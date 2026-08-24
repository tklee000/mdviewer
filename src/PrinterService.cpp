#include "PrinterService.h"

#include <windows.h>
#include <winspool.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

namespace printer {
namespace {

using Microsoft::WRL::ComPtr;
using winrt::Windows::Data::Pdf::PdfDocument;
using winrt::Windows::Data::Pdf::PdfPage;
using winrt::Windows::Data::Pdf::PdfPageRenderOptions;
using winrt::Windows::Storage::Streams::DataReader;
using winrt::Windows::Storage::Streams::DataWriter;
using winrt::Windows::Storage::Streams::InMemoryRandomAccessStream;

struct PrinterHandleCloser {
    void operator()(void* handle) const {
        if (handle) ClosePrinter(static_cast<HANDLE>(handle));
    }
};

struct DeviceContextCloser {
    void operator()(std::remove_pointer_t<HDC>* context) const {
        if (context) DeleteDC(context);
    }
};

using UniquePrinter = std::unique_ptr<void, PrinterHandleCloser>;
using UniqueDeviceContext =
    std::unique_ptr<std::remove_pointer_t<HDC>, DeviceContextCloser>;

struct RasterPage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<unsigned char> pixels;
};

std::wstring ErrorMessage(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (!length || !buffer) return L"Windows printing error " + std::to_wstring(code);
    std::wstring message(buffer, length);
    LocalFree(buffer);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' ||
            message.back() == L' ')) {
        message.pop_back();
    }
    return message;
}

std::wstring DefaultPrinterName() {
    DWORD size = 0;
    GetDefaultPrinterW(nullptr, &size);
    if (!size || size > 32768) return {};
    std::wstring name(size, L'\0');
    if (!GetDefaultPrinterW(name.data(), &size) || !size) return {};
    name.resize(size > 0 && name[size - 1] == L'\0' ? size - 1 : size);
    return name;
}

InMemoryRandomAccessStream PdfInputStream(
    const std::vector<unsigned char>& pdfBytes) {
    InMemoryRandomAccessStream stream;
    DataWriter writer(stream);
    writer.WriteBytes(winrt::array_view<const std::uint8_t>(
        pdfBytes.data(), pdfBytes.data() + pdfBytes.size()));
    writer.StoreAsync().get();
    writer.DetachStream();
    stream.Seek(0);
    return stream;
}

std::vector<unsigned char> ReadStream(InMemoryRandomAccessStream const& stream) {
    const std::uint64_t length = stream.Size();
    if (!length || length > std::numeric_limits<std::uint32_t>::max()) return {};
    stream.Seek(0);
    DataReader reader(stream.GetInputStreamAt(0));
    const std::uint32_t loaded = reader.LoadAsync(
        static_cast<std::uint32_t>(length)).get();
    if (loaded != length) return {};
    std::vector<unsigned char> bytes(loaded);
    reader.ReadBytes(winrt::array_view<std::uint8_t>(
        bytes.data(), bytes.data() + bytes.size()));
    reader.DetachStream();
    return bytes;
}

bool DecodeRenderedPage(IWICImagingFactory* factory,
                        std::vector<unsigned char>* encoded,
                        RasterPage* result,
                        std::wstring* error) {
    if (!factory || !encoded || encoded->empty() || !result) return false;
    ComPtr<IWICStream> stream;
    HRESULT value = factory->CreateStream(&stream);
    if (SUCCEEDED(value)) {
        value = stream->InitializeFromMemory(
            encoded->data(), static_cast<DWORD>(encoded->size()));
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(value)) {
        value = factory->CreateDecoderFromStream(
            stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(value)) value = decoder->GetFrame(0, &frame);
    ComPtr<IWICFormatConverter> converter;
    if (SUCCEEDED(value)) value = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(value)) {
        value = converter->Initialize(
            frame.Get(), GUID_WICPixelFormat32bppBGR,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom);
    }
    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(value)) value = converter->GetSize(&width, &height);
    const std::uint64_t stride = static_cast<std::uint64_t>(width) * 4;
    const std::uint64_t byteCount = stride * height;
    if (SUCCEEDED(value) && (!width || !height ||
        stride > std::numeric_limits<UINT>::max() ||
        byteCount > 256ull * 1024 * 1024 ||
        byteCount > std::numeric_limits<UINT>::max())) {
        value = E_OUTOFMEMORY;
    }
    if (SUCCEEDED(value)) {
        result->pixels.resize(static_cast<std::size_t>(byteCount));
        value = converter->CopyPixels(
            nullptr, static_cast<UINT>(stride), static_cast<UINT>(byteCount),
            result->pixels.data());
    }
    if (FAILED(value)) {
        if (error) *error = L"PDF page decoding failed (0x" +
            std::to_wstring(static_cast<unsigned long>(value)) + L")";
        return false;
    }
    result->width = width;
    result->height = height;
    return true;
}

std::pair<std::uint32_t, std::uint32_t> RenderSize(
    PdfPage const& page, int maximumWidth, int maximumHeight) {
    const auto media = page.Dimensions().MediaBox();
    const double width = (std::max)(1.0, static_cast<double>(media.Width));
    const double height = (std::max)(1.0, static_cast<double>(media.Height));
    const double scale = (std::min)(
        static_cast<double>((std::max)(1, maximumWidth)) / width,
        static_cast<double>((std::max)(1, maximumHeight)) / height);
    return {
        static_cast<std::uint32_t>((std::max)(1.0, std::round(width * scale))),
        static_cast<std::uint32_t>((std::max)(1.0, std::round(height * scale)))};
}

bool RenderPage(PdfPage const& page,
                IWICImagingFactory* factory,
                int maximumWidth,
                int maximumHeight,
                RasterPage* result,
                std::wstring* error) {
    const auto [width, height] = RenderSize(page, maximumWidth, maximumHeight);
    PdfPageRenderOptions options;
    options.DestinationWidth(width);
    options.DestinationHeight(height);
    options.BackgroundColor(winrt::Windows::UI::Colors::White());
    InMemoryRandomAccessStream output;
    page.RenderToStreamAsync(output, options).get();
    std::vector<unsigned char> encoded = ReadStream(output);
    if (encoded.empty()) {
        if (error) *error = L"Windows returned an empty rendered PDF page.";
        return false;
    }
    return DecodeRenderedPage(factory, &encoded, result, error);
}

bool ValidDeviceMode(const std::vector<unsigned char>& bytes) {
    if (bytes.size() < sizeof(DEVMODEW)) return false;
    const auto* mode = reinterpret_cast<const DEVMODEW*>(bytes.data());
    const std::size_t publicSize = mode->dmSize;
    const std::size_t privateSize = mode->dmDriverExtra;
    return publicSize >= sizeof(DEVMODEW) &&
        publicSize <= bytes.size() && privateSize <= bytes.size() - publicSize;
}

std::vector<unsigned char> CurrentPrinterDevMode(
    HWND ownerWindow,
    const std::wstring& printerName,
    HANDLE printerHandle,
    std::wstring* error) {
    const LONG required = DocumentPropertiesW(
        ownerWindow, printerHandle, const_cast<wchar_t*>(printerName.c_str()),
        nullptr, nullptr, 0);
    if (required <= 0 || required > 1024 * 1024) {
        if (error) *error = L"The selected printer did not provide valid settings.";
        return {};
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(required));
    auto* mode = reinterpret_cast<DEVMODEW*>(bytes.data());
    if (DocumentPropertiesW(
            ownerWindow, printerHandle,
            const_cast<wchar_t*>(printerName.c_str()), mode, nullptr,
            DM_OUT_BUFFER) != IDOK) {
        if (error) *error = L"The selected printer settings could not be read.";
        return {};
    }
    if (!ValidDeviceMode(bytes)) {
        if (error) *error = L"The selected printer returned invalid settings.";
        return {};
    }
    return bytes;
}

bool MergePrinterDevMode(HWND ownerWindow,
                         const std::wstring& printerName,
                         HANDLE printerHandle,
                         const std::vector<unsigned char>& input,
                         std::vector<unsigned char>* output,
                         std::wstring* error) {
    if (!output || output->empty() || !ValidDeviceMode(input)) return false;
    auto* outputMode = reinterpret_cast<DEVMODEW*>(output->data());
    auto* inputMode = reinterpret_cast<DEVMODEW*>(
        const_cast<unsigned char*>(input.data()));
    if (DocumentPropertiesW(
            ownerWindow, printerHandle,
            const_cast<wchar_t*>(printerName.c_str()), outputMode, inputMode,
            DM_IN_BUFFER | DM_OUT_BUFFER) != IDOK ||
        !ValidDeviceMode(*output)) {
        if (error) *error = L"The selected printer settings could not be applied.";
        return false;
    }
    return true;
}

std::vector<unsigned char> PrinterDevMode(const PrintOptions& options,
                                          HANDLE printerHandle,
                                          std::wstring* error) {
    std::vector<unsigned char> bytes = CurrentPrinterDevMode(
        nullptr, options.printerName, printerHandle, error);
    if (bytes.empty()) return {};
    if (!options.deviceMode.empty() &&
        !MergePrinterDevMode(nullptr, options.printerName, printerHandle,
                             options.deviceMode, &bytes, error)) {
        return {};
    }
    auto* mode = reinterpret_cast<DEVMODEW*>(bytes.data());
    mode->dmFields |= DM_ORIENTATION | DM_PAPERSIZE | DM_COPIES | DM_COLLATE;
    mode->dmOrientation = options.landscape
        ? DMORIENT_LANDSCAPE : DMORIENT_PORTRAIT;
    mode->dmPaperSize = options.paperWidthMillimeters > 212.0
        ? DMPAPER_LETTER : DMPAPER_A4;
    mode->dmCopies = static_cast<short>((std::min)(options.copies, 999u));
    mode->dmCollate = DMCOLLATE_TRUE;
    std::vector<unsigned char> normalized(bytes.size());
    if (!MergePrinterDevMode(nullptr, options.printerName, printerHandle,
                             bytes, &normalized, error)) {
        return {};
    }
    return normalized;
}

PrintResult LoadAndRender(const std::vector<unsigned char>& pdfBytes,
                          const PrintOptions* printOptions,
                          std::stop_token stopToken) {
    PrintResult result;
    if (pdfBytes.empty()) {
        result.error = L"The print preview is empty.";
        return result;
    }

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        const auto input = PdfInputStream(pdfBytes);
        const PdfDocument document = PdfDocument::LoadFromStreamAsync(input).get();
        result.pageCount = document.PageCount();
        if (!result.pageCount) {
            result.error = L"The print preview has no pages.";
            winrt::uninit_apartment();
            return result;
        }

        ComPtr<IWICImagingFactory> factory;
        winrt::check_hresult(CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory)));

        UniqueDeviceContext printerDc;
        int physicalWidth = 1600;
        int physicalHeight = 2200;
        int offsetX = 0;
        int offsetY = 0;
        if (printOptions) {
            HANDLE opened = nullptr;
            if (!OpenPrinterW(
                    const_cast<wchar_t*>(printOptions->printerName.c_str()),
                    &opened, nullptr) || !opened) {
                result.error = ErrorMessage(GetLastError());
                winrt::uninit_apartment();
                return result;
            }
            UniquePrinter printerHandle(opened);
            std::vector<unsigned char> modeBytes = PrinterDevMode(
                *printOptions, printerHandle.get(), &result.error);
            if (modeBytes.empty()) {
                winrt::uninit_apartment();
                return result;
            }
            auto* mode = reinterpret_cast<DEVMODEW*>(modeBytes.data());
            printerDc.reset(CreateDCW(
                L"WINSPOOL", printOptions->printerName.c_str(), nullptr, mode));
            if (!printerDc) {
                result.error = ErrorMessage(GetLastError());
                winrt::uninit_apartment();
                return result;
            }
            physicalWidth = GetDeviceCaps(printerDc.get(), PHYSICALWIDTH);
            physicalHeight = GetDeviceCaps(printerDc.get(), PHYSICALHEIGHT);
            offsetX = GetDeviceCaps(printerDc.get(), PHYSICALOFFSETX);
            offsetY = GetDeviceCaps(printerDc.get(), PHYSICALOFFSETY);
            if (physicalWidth <= 0 || physicalHeight <= 0) {
                result.error = L"The selected printer did not report a page size.";
                winrt::uninit_apartment();
                return result;
            }
            DOCINFOW documentInfo{sizeof(documentInfo)};
            documentInfo.lpszDocName = printOptions->documentName.empty()
                ? L"MdViewer document" : printOptions->documentName.c_str();
            if (StartDocW(printerDc.get(), &documentInfo) <= 0) {
                result.error = ErrorMessage(GetLastError());
                winrt::uninit_apartment();
                return result;
            }
        }

        bool documentStarted = printOptions != nullptr;
        const double renderScale = (std::min)(
            1.0, 300.0 / (std::max)(72, printOptions
                ? GetDeviceCaps(printerDc.get(), LOGPIXELSX) : 300));
        const int maximumWidth = (std::min)(
            5000, (std::max)(1, static_cast<int>(physicalWidth * renderScale)));
        const int maximumHeight = (std::min)(
            5000, (std::max)(1, static_cast<int>(physicalHeight * renderScale)));

        for (std::uint32_t index = 0; index < result.pageCount; ++index) {
            if (stopToken.stop_requested()) {
                if (documentStarted) AbortDoc(printerDc.get());
                result.error = L"Printing was canceled.";
                winrt::uninit_apartment();
                return result;
            }
            const PdfPage page = document.GetPage(index);
            RasterPage raster;
            if (!RenderPage(page, factory.Get(), maximumWidth, maximumHeight,
                            &raster, &result.error)) {
                if (documentStarted) AbortDoc(printerDc.get());
                winrt::uninit_apartment();
                return result;
            }
            page.Close();

            if (!printOptions) continue;
            if (StartPage(printerDc.get()) <= 0) {
                result.error = ErrorMessage(GetLastError());
                AbortDoc(printerDc.get());
                winrt::uninit_apartment();
                return result;
            }
            const double sourceAspect = static_cast<double>(raster.width) /
                static_cast<double>(raster.height);
            const double destinationAspect = static_cast<double>(physicalWidth) /
                static_cast<double>(physicalHeight);
            int destinationWidth = physicalWidth;
            int destinationHeight = physicalHeight;
            if (sourceAspect > destinationAspect) {
                destinationHeight = static_cast<int>(std::round(
                    physicalWidth / sourceAspect));
            } else {
                destinationWidth = static_cast<int>(std::round(
                    physicalHeight * sourceAspect));
            }
            const int destinationX = -offsetX +
                (physicalWidth - destinationWidth) / 2;
            const int destinationY = -offsetY +
                (physicalHeight - destinationHeight) / 2;
            BITMAPINFO bitmapInfo{};
            bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(raster.width);
            bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(raster.height);
            bitmapInfo.bmiHeader.biPlanes = 1;
            bitmapInfo.bmiHeader.biBitCount = 32;
            bitmapInfo.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(printerDc.get(), HALFTONE);
            SetBrushOrgEx(printerDc.get(), 0, 0, nullptr);
            const int copied = StretchDIBits(
                printerDc.get(), destinationX, destinationY,
                destinationWidth, destinationHeight, 0, 0,
                static_cast<int>(raster.width), static_cast<int>(raster.height),
                raster.pixels.data(), &bitmapInfo, DIB_RGB_COLORS, SRCCOPY);
            if (copied == GDI_ERROR || copied == 0 || EndPage(printerDc.get()) <= 0) {
                result.error = ErrorMessage(GetLastError());
                AbortDoc(printerDc.get());
                winrt::uninit_apartment();
                return result;
            }
        }

        if (documentStarted && EndDoc(printerDc.get()) <= 0) {
            result.error = ErrorMessage(GetLastError());
            winrt::uninit_apartment();
            return result;
        }
        result.success = true;
        winrt::uninit_apartment();
        return result;
    } catch (const winrt::hresult_error& exception) {
        result.error = exception.message().c_str();
    } catch (const std::exception& exception) {
        result.error.assign(exception.what(), exception.what() +
            std::char_traits<char>::length(exception.what()));
    }
    try { winrt::uninit_apartment(); } catch (...) {}
    return result;
}

}  // namespace

std::vector<PrinterInfo> ListPrinters() {
    DWORD required = 0;
    DWORD returned = 0;
    constexpr DWORD flags = PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS;
    EnumPrintersW(flags, nullptr, 4, nullptr, 0, &required, &returned);
    if (!required || required > 32 * 1024 * 1024) return {};
    std::vector<unsigned char> bytes(required);
    if (!EnumPrintersW(flags, nullptr, 4, bytes.data(), required,
                       &required, &returned)) {
        return {};
    }
    const std::wstring defaultName = DefaultPrinterName();
    const auto* information =
        reinterpret_cast<const PRINTER_INFO_4W*>(bytes.data());
    std::vector<PrinterInfo> printers;
    printers.reserve(returned);
    for (DWORD index = 0; index < returned; ++index) {
        if (!information[index].pPrinterName ||
            !*information[index].pPrinterName) continue;
        PrinterInfo entry;
        entry.name = information[index].pPrinterName;
        entry.isDefault = !_wcsicmp(entry.name.c_str(), defaultName.c_str());
        printers.push_back(std::move(entry));
    }
    std::sort(printers.begin(), printers.end(),
              [](const PrinterInfo& left, const PrinterInfo& right) {
                  if (left.isDefault != right.isDefault) return left.isDefault;
                  return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
              });
    printers.erase(std::unique(
        printers.begin(), printers.end(),
        [](const PrinterInfo& left, const PrinterInfo& right) {
            return !_wcsicmp(left.name.c_str(), right.name.c_str());
        }), printers.end());
    return printers;
}

PrinterPropertiesResult ShowPrinterProperties(
    void* ownerWindow,
    const std::wstring& printerName,
    const std::vector<unsigned char>& initialDeviceMode) {
    PrinterPropertiesResult result;
    if (printerName.empty() || printerName.size() > 1024) {
        result.error = L"The selected printer is invalid.";
        return result;
    }
    HANDLE opened = nullptr;
    if (!OpenPrinterW(const_cast<wchar_t*>(printerName.c_str()),
                      &opened, nullptr) || !opened) {
        result.error = ErrorMessage(GetLastError());
        return result;
    }
    UniquePrinter printerHandle(opened);
    const HWND owner = static_cast<HWND>(ownerWindow);
    std::vector<unsigned char> settings = CurrentPrinterDevMode(
        owner, printerName, printerHandle.get(), &result.error);
    if (settings.empty()) return result;
    if (!initialDeviceMode.empty() &&
        !MergePrinterDevMode(owner, printerName, printerHandle.get(),
                             initialDeviceMode, &settings, &result.error)) {
        return result;
    }
    auto* mode = reinterpret_cast<DEVMODEW*>(settings.data());
    const LONG response = DocumentPropertiesW(
        owner, printerHandle.get(), const_cast<wchar_t*>(printerName.c_str()),
        mode, mode, DM_IN_PROMPT | DM_IN_BUFFER | DM_OUT_BUFFER);
    if (response == IDCANCEL) {
        result.status = PrinterPropertiesStatus::Canceled;
        return result;
    }
    if (response != IDOK || !ValidDeviceMode(settings)) {
        result.error = L"The printer driver settings could not be opened.";
        return result;
    }
    result.status = PrinterPropertiesStatus::Applied;
    result.deviceMode = std::move(settings);
    return result;
}

PrintResult PrintPdf(const std::vector<unsigned char>& pdfBytes,
                     const PrintOptions& options,
                     std::stop_token stopToken) {
    if (options.printerName.empty() || options.printerName.size() > 1024 ||
        options.copies < 1 || options.copies > 999) {
        return {false, 0, L"The print settings are invalid."};
    }
    return LoadAndRender(pdfBytes, &options, stopToken);
}

PrintResult ValidatePdf(const std::vector<unsigned char>& pdfBytes,
                        std::stop_token stopToken) {
    return LoadAndRender(pdfBytes, nullptr, stopToken);
}

}  // namespace printer
