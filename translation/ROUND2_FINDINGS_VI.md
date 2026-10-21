# Rà soát bản dịch Ragnarok Violet WIPI – vòng 2

## Các vị trí xác định từ ảnh chụp melonDS

| Nội dung còn sót | ELF VA | Sau khi sửa |
|---|---|---|
| Cảnh mở đầu | `0x3B380` | Đoạn mở đầu tiếng Anh, tên **Clide**, học viện Violet Knights (301/322 byte) |
| Cảnh kết thúc truyện | `0x3B1B8` | Rút gọn nội dung tiếng Anh theo giới hạn dữ liệu |
| Cửa sổ nhận nhiệm vụ | `0x3D9E4` | `New Quest!` |
| Hoàn thành nhiệm vụ | `0x3D9F4` | `Quest Done!` |
| NPC chào | `0x14015D8` | `Hi Clide` |
| NPC mẹ – hai bảng tên | `0x141237C`, `0x14139B0` | `Mom` |
| Menu Setup | `0x1400A0E`–`0x1400A3B` | `Save`, `Lux Shop`, `Settings`, `Help`, `Records`, `Menu` |
| Cửa sổ xác nhận | `0x14008D9`, `0x14008E0` | `OK`, `No` |
| Trang INFO | `0x3C330` | Nội dung tiếng Anh, giữ dấu `|` ngắt dòng (152/158 byte) |

**Thuật ngữ:** Dùng **Clide** vì bảng tiếng Anh iOS có `Go Clide!!` và các khóa `CLIDE_*`, dù tên phiên âm có thể thường được viết là Clyde.

## Phương pháp

Dò lại `translation/wipi_native_remaining.csv` theo chuỗi tiếng Hàn thực sự xuất hiện trên ảnh, khớp `elf_va` và nội dung CP949 trong `nitrofs/binary.mod`. Chỉ thay trong đúng vùng cấp phát chuỗi (ASCII + NUL padding), không thay đổi độ dài tệp ELF, bảng relocation, hay địa chỉ section. Dò byte-diff giữa tệp trước/sau, bảo đảm không sửa ngoài các vùng cho phép. Các lệnh điều khiển gốc không thay đổi.

## Kết quả và giới hạn

- Vá **146 chuỗi ELF**, trong đó có đủ các vùng chỉ ra trên ảnh. Tổng **2.392 byte** thay đổi so với bản trước.
- Còn **1.088 entry tiếng Hàn đã nhận diện trong ELF** chưa xử lý; một số là tên riêng quá dài so với ô chuỗi, hoặc cần xác minh ngữ cảnh. Xem `translation/wipi_native_remaining.csv` và `translation/round2_native_patch_report.json`.
- Static test **PASS**. Chưa build ROM `.nds` hoặc chạy melonDS ở môi trường hiện tại. Chưa thể bảo đảm từng dòng hiển thị đúng độ rộng; cần kiểm tra trên ROM mới.
- Không có chỉnh sửa PNG, tệp thoại WIPI hoặc cơ chế save.

## Hướng dẫn

Dùng gói dự án đầy đủ hoặc giải nén bản vá riêng ghi đè theo đúng đường dẫn `VioletDS-English/`, sau đó chạy `build.bat` và kiểm tra lại 7 màn hình đã báo lỗi. Nếu còn sót, cung cấp ảnh chụp và vị trí trong game để truy về chuỗi cùng nhóm.
