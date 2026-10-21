# Ragnarok Violet – WIPI / VioletDS: bản chuyển tiếng Anh

## Phạm vi hoàn thành

- Đối chiếu **479/479** tệp `res/diaData/` theo cấu trúc và từng entry của bản iOS. Một tệp đặc biệt là **185**: WIPI có **6 entry**, iOS có **4 entry**; đã dịch riêng 2 entry đầu và căn lại 4 entry còn lại.
- Chép **2.145** đoạn hội thoại gốc tiếng Anh iOS, dịch tay **68** entry WIPI không có bản tương ứng (gồm 66 `NONE` và 2 câu tệp 185).
- **370 entry lệnh** (biểu cảm `HEAD_*`, sepia) giữ mã tiếng Hàn của WIPI để game xử lý đúng.
- Sửa lời hướng dẫn **Touch / Tap / Click** ở hội thoại thành điều khiển WIPI; bố trí phím gốc vẫn do mã nguồn VioletDS quyết định.
- Xử lý bảng mô tả `res/0.dat`..`res/4.dat`: **428/429** entry đổi tiếng Anh, **1 entry trống** giữ nguyên. Ưu tiên so khớp nguồn tiếng Hàn với `English.lproj/Localizable.strings` của iOS (1.845 khóa), dịch phần không có bản iOS.
- Trong **1.972 entry** tĩnh của `binary.mod`, đã đổi **683 entry** vừa vùng cấp phát gốc; **1.234 entry tiếng Hàn** còn lại **chưa được thay thế** do vượt sức chứa, không có bản khớp chắc chắn, hoặc là chuỗi điều khiển. Chưa có cơ chế repoint ELF an toàn trong bộ công cụ hiện tại.
- Tổng cộng **3.310 entry được sửa trong 484 tệp**. Ngoài các tệp text/ELF trên, mọi tài nguyên khác đều có SHA-256 giống dự án gốc.

## Giới hạn và kiểm thử

- **Chưa phải bản 100% tiếng Anh**: tên đồ, NPC, kỹ năng và một số màn hình đang nằm trong ELF vẫn có thể hiện tiếng Hàn. Tệp `translation/wipi_native_remaining.csv` liệt kê những entry chưa được dịch trong ELF.
- `binary.mod` chỉ được thay chuỗi trong vùng byte được xác minh; không thay offset lệnh, mã hội thoại, event code, hay font. Có thể cần hệ thống repoint ELF/hook renderer nếu muốn chuyển hết phần UI.
- Đã chạy kiểm tra cấu trúc của `ragnarok_violet_text_tool.py` (**PASS**) và đối chiếu số entry/portrait code trong từng tệp thoại.
- **Chưa build ROM `.nds`, chưa chạy game trên DS/melonDS** vì môi trường không có devkitARM. Vẫn cần kiểm tra hiển thị thực tế, tự xuống dòng và tràn giao diện, đặc biệt thoại dài.

## Điều khiển WIPI trong dự án VioletDS hiện tại

| NDS | WIPI |
|---|---|
| D-pad | Di chuyển / 2,4,6,8 |
| A | EZ / 5 / xác nhận / tấn công |
| B | CLR / menu |
| X | 0 / pet |
| Y | * |
| R | # |
| L+A | 1 / đổi vũ khí |
| L+B | 3 / minimap |
| L+X | 7 |
| L+Y | 9 |
| START | CALL / lưu |

## Build trên Windows

Chạy `build.bat` của dự án như trước (đòi hỏi devkitPro/devkitARM). Không cần IPA iOS khi chạy bản WIPI đã vá.

## Tài liệu kèm theo

- `translation/wipi_native_remaining.csv`: nguồn tiếng Hàn và các phương án tiếng Anh chưa thể ghi đè an toàn.
- `translation/translated_entries.jsonl`: source/target và metadata của các entry đã đối chiếu, để tiếp tục phát triển phần repoint ELF.
- `translation/ios_english_catalog.json`: bảng khóa tiếng Anh gốc iOS (tên item/quest/skill/map/UI).
- `translation/static_validation.json`: thống kê kiểm tra/đối chiếu tệp.
