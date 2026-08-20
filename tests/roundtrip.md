---
title: MdViewer round-trip fixture
language: ko-KR
---

# Markdown 편집 테스트

일반 문단에 **굵은 글씨**, *기울임꼴*, `inline code`, [링크](https://example.com)가 있습니다.

> 인용문도 미리보기에서 편집할 수 있어야 합니다.

## 목록

- 첫 번째 항목
- [ ] 아직 하지 않은 작업
- [x] 완료한 작업

1. 첫 번째 순서
2. 두 번째 순서

## 코드

```cpp
#include <iostream>

int main() {
    std::cout << "안녕하세요" << std::endl;
}
```

## 표

| 기능 | 상태 |
| --- | --- |
| 소스 편집 | 지원 |
| 미리보기 편집 | 지원 |

![상대 경로 이미지](images/sample.png)
