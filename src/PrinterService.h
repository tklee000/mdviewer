#pragma once

#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

namespace printer {

struct PrinterInfo {
    std::wstring name;
    bool isDefault = false;
};

struct PrintOptions {
    std::wstring printerName;
    std::wstring documentName;
    std::uint32_t copies = 1;
    double paperWidthMillimeters = 210.0;
    double paperHeightMillimeters = 297.0;
    bool landscape = false;
    std::vector<unsigned char> deviceMode;
};

enum class PrinterPropertiesStatus {
    Applied,
    Canceled,
    Failed,
};

struct PrinterPropertiesResult {
    PrinterPropertiesStatus status = PrinterPropertiesStatus::Failed;
    std::vector<unsigned char> deviceMode;
    std::wstring error;
};

struct PrintResult {
    bool success = false;
    std::uint32_t pageCount = 0;
    std::wstring error;
};

std::vector<PrinterInfo> ListPrinters();

PrinterPropertiesResult ShowPrinterProperties(
    void* ownerWindow,
    const std::wstring& printerName,
    const std::vector<unsigned char>& initialDeviceMode = {});

PrintResult PrintPdf(const std::vector<unsigned char>& pdfBytes,
                     const PrintOptions& options,
                     std::stop_token stopToken = {});

// Exercises the same Windows PDF rasterization and WIC decoding path without
// creating a spooler job. Used by the native smoke test.
PrintResult ValidatePdf(const std::vector<unsigned char>& pdfBytes,
                        std::stop_token stopToken = {});

}  // namespace printer
