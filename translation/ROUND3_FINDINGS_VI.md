# Ragnarok Violet WIPI — Round 3: bổ sung tiếng Anh menu, nhân vật, tên đồ

## Nguồn và phương pháp

Tiếp tục từ bản **Round 2** (bản iOS làm nguồn tham chiếu tiếng Anh). Đối chiếu các ảnh do người dùng cung cấp và tài nguyên iOS để dò những chuỗi tiếng Hàn chưa dịch nằm trong `nitrofs/binary.mod`:

- Character: `소드맨` → `Swdman` (viết tắt *Swordsman*); chỉ dẫn tăng ATK → `ATK increased`.
- Equipment: `단검` → `Dag.` (*Dagger*), do chuỗi chỉ có 4 byte; đây là loại/nhãn, không phải bản dịch đầy đủ của tên item.
- Inventory: `재료주머니` → `Mat. Pouch`; phần mô tả dài được sửa thành `Stores materials.`.
- Skill: `대거 쉴드` → `Dag Shield` (*Dagger Shield*); mô tả skill đầu → `Spin daggers around you.`.
- Help: `조작` → `Keys` và các tiêu đề khác (`Stats`, `Quick Slots`, `Network`, v.v.).
- Bảng dài còn lại: quest, khu vực, công trình, quái vật, NPC, equipment, consumable, menu và một số lời nói của pet.

Các tên tiếng Anh lấy từ bảng localization iOS khi có bản khớp; nếu vượt dung lượng cố định của tên WIPI, sử dụng **viết tắt nhất quán**, không dịch nguyên văn quá giới hạn rồi để game cắt chuỗi.

## Số liệu

- Lượt Round 3: **1.060 entry ELF** mới được chuyển từ tiếng Hàn sang tiếng Anh hoặc viết tắt, trong đó 851 entry lượt đầu, 199 entry bổ sung, 10 entry rà cuối.
- Có thêm **15 hiệu đính** tên viết tắt và tên riêng, **2 hiệu đính** dòng credit tiếng Anh, và **2 entry mô tả** (`res/2.dat`, `res/3.dat`) được viết ngắn để giảm tràn màn hình (có thêm bước chỉnh cuối tại cùng vị trí).
- **28 entry** trong bảng kiểm kê ELF vẫn còn tiếng Hàn sau Round 3. Gồm 15 entry rất ngắn 2 byte, mã điều khiển biểu cảm/sepia trong game, nhãn sự kiện nội bộ và 1 câu không có đối chiếu đáng tin. Không ép thay tên lệnh để tránh hỏng event.
- Đã kiểm tra static test: PASS. **Chưa build ROM thật** (môi trường hiện tại không có devkitARM); chưa chơi bản mới trên melonDS để chứng nhận hiển thị và save.

## An toàn patch

- In-place substitution theo đúng ELF virtual address -> file offset; xác minh byte CP949 gốc trước khi thay.
- Chỉ ghi tối đa số byte có sẵn của mỗi chuỗi, xóa phần byte thừa bằng NUL; **không repoint**, không di chuyển section/relocation và không đổi kích thước ELF (474.040 byte).
- Bảo vệ các placeholder như `%s`, định dạng phân tách `|` trong thông báo có cấu trúc.
- Các tệp tài nguyên không thuộc `binary.mod`, `res/2.dat`, `res/3.dat` không thay đổi so với Round 2 (ngoại trừ tài liệu kiểm kê/báo cáo).
- Các chuỗi giá dịch vụ **WIPI đời cũ** được gắn nhãn legacy; chúng không phải giá/phí áp dụng trên NDS.

## Xem và thử nghiệm

1. Để build: giải nén gói full rồi chạy `build.bat` với devkitPro + devkitARM như các bản trước.
2. Với dự án Round 2 đang dùng: giải nén gói patch-only đè vào thư mục cùng tên `VioletDS-English`.
3. Mở melonDS, kiểm tra từ đầu game 5 tab: **Char → Equip → Inventory → Skill → Setup/Help**, thêm tên NPC, title bản đồ và quest sau khi chuyển map.
4. Danh sách còn lại: `translation/wipi_native_remaining.csv`.
5. Biên bản chi tiết: `translation/round3_native_patch_report.json`, `round3b_native_patch_report.json`, `round3c_native_patch_report.json`, `round3_qa_corrections.json`.

**Lưu ý chất lượng:** Một tên ngắn chỉ 2–4 byte đôi khi bắt buộc rất khó nhận biết (`Dag.`, `Pori`, `Kar.`...). Nếu cần tất cả tên đầy đủ, hướng kỹ thuật kế tiếp là repoint ELF hoặc bổ sung runtime string pool cùng thử nghiệm thực tế. Bản này cố ý không thử điều đó.
