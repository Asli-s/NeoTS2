// UI scripts and layout, logs and crash reports.
#include "test_support.hpp"

#include "../app/click_gate.hpp"

#include "ui/animated_bitmap.hpp"
#include "ui/drag_arrow.hpp"
#include "ui/scroll_bar.hpp"
#include "ui/tip_layout.hpp"

namespace {

// A hover tip goes above its button when there is room, centred and kept 16 pixels inside the
// screen. Near the top it goes below, and at a side edge beside the button.
void tipsArePlacedAsTheGame() {
    using ts2::ui::placeTip;
    const std::array<int, 2> hd{1920, 1080};
    const auto is = [](ts2::ui::TipBox b, int x, int y, int w, int h) { return b.x == x && b.y == y && b.w == w && b.h == h; };
    CHECK(is(placeTip(384, 866, 122, 48, hd), 323, 810, 122, 48));  // a three-line want tip, as the game placed it
    CHECK(is(placeTip(26, 659, 74, 24, hd), 16, 627, 74, 24));      // held 16 pixels in from the left
    CHECK(is(placeTip(1910, 500, 74, 24, hd), 1828, 488, 74, 24));  // at the right edge: to the left
    CHECK(is(placeTip(18, 500, 74, 24, hd), 26, 488, 74, 24));      // at the left edge: to the right
    CHECK(is(placeTip(500, 30, 74, 24, hd), 463, 38, 74, 24));      // near the top: below
    CHECK(is(placeTip(500, 500, 10, 4, hd), 491, 483, 18, 9));      // the frame's smallest box
    // A tip over a Sim in the world keeps 28 pixels from its spot and 8 from the screen's top left.
    const std::array<int, 4> world{8, 8, 1904, 1064};
    CHECK(is(placeTip(1200, 500, 120, 23, world, 28), 1140, 449, 120, 23));
    CHECK(is(placeTip(30, 500, 120, 23, world, 28), 8, 449, 120, 23));
    CHECK(is(placeTip(1200, 40, 120, 23, world, 28), 1140, 68, 120, 23));  // near the top: below
}

// Up to 12 entries share one pie. With more, each page shows "More..." and 11 of them, and the last
// page's "More..." goes back to the first.
// While a scene loads, presses are dropped and so are their releases, even once loading has ended.
// A button pressed before loading keeps its release, and other buttons aren't touched.
void clicksAreDroppedWhileLoading() {
    app::ClickGate gate;
    constexpr unsigned kLeft = 0, kRight = 2;
    CHECK(!gate.drops(true, kLeft, false) && !gate.drops(false, kLeft, false));  // not loading
    CHECK(!gate.drops(true, kRight, false));
    CHECK(gate.drops(true, kLeft, true));    // pressed while loading
    CHECK(!gate.drops(false, kRight, true));  // pressed before loading
    CHECK(gate.drops(false, kLeft, false));   // released after loading ended
    CHECK(!gate.drops(true, kLeft, false) && !gate.drops(false, kLeft, false));  // the next click goes through
    CHECK(!gate.drops(true, 99, true));       // a button past the 8 it tracks goes through
}

void piePagesHoldElevenAndMore() {
    namespace pie = ts2::ui::pie;
    auto same = [](const pie::Page& p, std::size_t first, std::size_t count, bool more, std::size_t next) {
        return p.first == first && p.count == count && p.more == more && p.next == next;
    };
    CHECK(same(pie::page(0, 0), 0, 0, false, 0));
    CHECK(same(pie::page(12, 0), 0, 12, false, 0));
    CHECK(same(pie::page(13, 0), 0, 11, true, 11));
    CHECK(same(pie::page(13, 11), 11, 2, true, 0));
    CHECK(same(pie::page(30, 11), 11, 11, true, 22));
    CHECK(same(pie::page(30, 22), 22, 8, true, 0));
    CHECK(same(pie::page(22, 11), 11, 11, true, 0));  // a full later page: its "More..." goes back
    CHECK(same(pie::page(5, 40), 5, 0, true, 0));     // past the end: nothing but "More..."
}

// The pie menu's items go round the mouse clockwise from the top, their knobs 68 pixels out, the
// knob's side and the pill's place set by the item's angle. The menu stays on screen.
void pieItemsGoRoundTheCentre() {
    namespace pie = ts2::ui::pie;
    const std::vector<int> one{40};
    auto menu = pie::layout(500, 400, one, 16, 250, 370, 1920, 1080);
    CHECK(menu.items.size() == 1 && menu.items[0].shape == pie::Shape::Below);
    CHECK(menu.items[0].knobX == 500 && menu.items[0].knobY == 332);
    // Text 40 wide: two middles of (40 - 35) / 2, the stalk and two caps, 48 above the knob.
    CHECK(menu.items[0].rect.w == 16 + 2 + 35 + 2 + 16 && menu.items[0].rect.x == 500 - 71 / 2 && menu.items[0].rect.y == 332 - 48);

    const std::vector<int> two{40, 60};
    menu = pie::layout(500, 400, two, 16, 250, 370, 1920, 1080);
    CHECK(menu.items[1].shape == pie::Shape::Above && menu.items[1].knobY == 468 && menu.items[1].rect.y == 468 - 21);
    CHECK(menu.items[1].textY == (42 - 16) / 2 + 26);

    const std::vector<int> seven{50, 50, 50, 50, 50, 50, 50};
    menu = pie::layout(500, 400, seven, 16, 250, 370, 1920, 1080);
    const pie::Shape shapes[] = {pie::Shape::Below, pie::Shape::LeftSide, pie::Shape::LeftSide, pie::Shape::LeftLower,
                                 pie::Shape::RightLower, pie::Shape::RightSide, pie::Shape::RightSide};
    for (std::size_t i = 0; i < 7; ++i) {
        CHECK(menu.items[i].shape == shapes[i]);
    }
    // A knob on the left: the item starts 14 left of it and is the text plus 54 wide, text at 38.
    CHECK(menu.items[1].rect.x == menu.items[1].knobX - 14 && menu.items[1].rect.w == 104 && menu.items[1].textX == 38);
    CHECK(menu.items[5].rect.x == menu.items[5].knobX + 14 - 104 && menu.items[5].textX == 16);

    // Over an item it is picked. Near the centre or far away nothing is. Elsewhere the nearest direction is.
    CHECK(pie::pick(menu, menu.items[1].rect.x + 60, menu.items[1].rect.y + 20) == std::optional<std::size_t>(1));
    CHECK(!pie::pick(menu, 510, 410));
    CHECK(!pie::pick(menu, 1900, 1000));
    CHECK(pie::pick(menu, 500, 340) == std::optional<std::size_t>(0));
    // A stalked item is only its pill and knob: not the corners beside its stalk.
    const auto& top = menu.items[0];
    CHECK(pie::over(top, top.rect.x + 1, top.rect.y + 1) && pie::over(top, top.knobX, top.rect.y + pie::kStalkHeight - 1));
    CHECK(!pie::over(top, top.rect.x + 1, top.rect.y + pie::kStalkHeight - 1));
    // With one item at the top, the mouse below the centre picks nothing.
    const std::vector<int> lone{40};
    const auto single = pie::layout(500, 400, lone, 16, 250, 370, 1920, 1080);
    CHECK(!pie::pick(single, 500, 500) && pie::pick(single, 560, 380) == std::optional<std::size_t>(0));

    // Opened at the screen's corner, the whole menu moves back on, 1 pixel in.
    menu = pie::layout(5, 5, seven, 16, 250, 370, 1920, 1080);
    CHECK(menu.bounds.x == 1 && menu.bounds.y == 1 && menu.centreX > 5 && menu.centreY > 5);
}

// A UI script: windows with their attributes, quoted text with entities, lists and keys, and a
// window's children.
void uiScriptsParse() {
    const std::string_view script =
        "# Generated by UI editor\r\n"
        "<LEGACY clsid=GZWinGen iid=IGZWinGen id=0xcda360ce area=(137,85,839,647) caption=\"Main menu\" winflag_visible=yes >\r\n"
        "<CHILDREN>\r\n"
        "   <LEGACY clsid=GZWinBtn iid=IGZWinBtn id=0x000000a2 area=(-26,19,91,72) caption=\"Say &quot;hi&quot; &amp; go\" "
        "image={499db772,ccc30090} captionres={7f3a262a,00840025} winflag_visible=no >\r\n"
        "   <CHILDREN>\r\n"
        "      <LEGACY clsid=GZWinText iid=IGZWinText area=(0,0,10,10) >\r\n"
        "   </CHILDREN>\r\n"
        "</CHILDREN>\r\n"
        "<LEGACY clsid=GZWinBMP iid=IGZWinBMP area=(0,0,1,1) >\r\n";
    const auto windows = ts2::ui::parse(script);
    CHECK(windows.size() == 2 && windows[0].children.size() == 1 && windows[0].children[0].children.size() == 1);
    // Windows nested far deeper than any of the game's scripts are refused rather than overflowing
    // the stack.
    std::string deep;
    for (int i = 0; i < 10000; ++i) {
        deep += "<LEGACY clsid=GZWinGen >\r\n<CHILDREN>\r\n";
    }
    bool refused = false;
    try {
        ts2::ui::parse(deep);
    } catch (const std::exception&) {
        refused = true;
    }
    CHECK(refused);
    const auto& root = windows[0];
    CHECK(root.text("iid") == "IGZWinGen" && root.number("id") == 0xcda360ceu && root.flag("winflag_visible"));
    CHECK((root.list("area") == std::vector<std::int64_t>{137, 85, 839, 647}));
    const auto& button = root.children[0];
    CHECK(button.text("caption") == "Say \"hi\" & go" && !button.flag("winflag_visible"));
    CHECK((button.list("area") == std::vector<std::int64_t>{-26, 19, 91, 72}));
    CHECK((button.list("image") == std::vector<std::int64_t>{0x499db772, 0xccc30090}));
    CHECK((button.list("captionres") == std::vector<std::int64_t>{0x7f3a262a, 0x00840025}));  // leakcheck: ok
    CHECK(windows[1].text("iid") == "IGZWinBMP" && !windows[1].get("caption"));
}

// The run's log: every line in its file, the last kRecentLines kept in memory (cut to fit), and only the last
// ten logs kept in its folder.
void logKeepsItsLinesAndFiles() {
    namespace fs = std::filesystem;
    ts2::diag::reset();
    const fs::path folder = fs::temp_directory_path() / ("neots2-log-test-" + std::to_string(std::random_device{}()));
    fs::create_directories(folder);
    for (int i = 0; i < 12; ++i) {  // older runs' logs
        std::ofstream(folder / ("neots2-20000101-0000" + std::to_string(10 + i) + ".log")) << "old";
    }
    CHECK(ts2::diag::open(folder, "NeoTS2 test"));
    ts2::diag::step("lot opened");
    ts2::diag::error(std::string(400, 'x'));
    std::size_t logs = 0;
    for (const auto& e : fs::directory_iterator(folder)) {
        logs += e.path().filename().string().rfind("neots2-", 0) == 0 ? 1 : 0;
    }
    CHECK(logs == ts2::diag::kKeptLogs && fs::exists(ts2::diag::file()));
    std::string text;
    {
        std::ifstream in(ts2::diag::file());
        text.assign(std::istreambuf_iterator<char>(in), {});
    }
    CHECK(text.rfind("NeoTS2 test\n", 0) == 0 && text.find("step: lot opened") != std::string::npos);
    CHECK(text.find(std::string(400, 'x')) != std::string::npos);  // whole in the file
    auto recent = ts2::diag::recentLines();
    CHECK(recent.size() == 2 && recent[0].find("lot opened") != std::string::npos && recent[1].size() < ts2::diag::kLineLength);
    // 36 more lines than are kept: the oldest 36 go.
    const std::size_t written = ts2::diag::kRecentLines + 36;
    for (std::size_t i = 0; i < written; ++i) {
        ts2::diag::info("line " + std::to_string(i));
    }
    recent = ts2::diag::recentLines();
    CHECK(recent.size() == ts2::diag::kRecentLines && recent.front().find("line 36") != std::string::npos &&
          recent.back().find("line " + std::to_string(written - 1)) != std::string::npos);
    char small[10];
    CHECK(ts2::diag::recentLines(small, sizeof small) == 9 && small[9] == '\0');
    ts2::diag::reset();

    // The user's own folder is written as %USERPROFILE% in the header and the lines, and their
    // name as a folder elsewhere as %USERNAME%.
    ts2::diag::hideUser({"C:\\Users\\Sam", {"Sam"}});  // a made-up user, leakcheck: ok
    CHECK(ts2::diag::open(folder, "command line: \"C:\\Users\\Sam\\NeoTS2\\neots2.exe\" \"D:\\SAM\\saves\""));  // leakcheck: ok
    ts2::diag::info("save folder C:/users/sam/Documents/EA Games and C:\\Users\\Samantha");  // leakcheck: ok
    {
        std::ifstream in(ts2::diag::file());
        text.assign(std::istreambuf_iterator<char>(in), {});
    }
    CHECK(text.find("Sam\\") == std::string::npos && text.find("sam/") == std::string::npos);
    CHECK(text.find("\"%USERPROFILE%\\NeoTS2\\neots2.exe\" \"D:\\%USERNAME%\\saves\"") != std::string::npos &&
          text.find("%USERPROFILE%/Documents/EA Games and C:\\Users\\Samantha") != std::string::npos);  // leakcheck: ok
    CHECK(ts2::diag::recentLines().back().find("%USERPROFILE%/Documents") != std::string::npos);
    ts2::diag::reset();
    std::error_code ec;
    fs::remove_all(folder, ec);
}

// A crash report names what happened and where. Its issue link keeps to module names and offsets
// (no folders, no log lines) and stays short enough for a browser.
void crashReportsSayWhatHappened() {
    using namespace ts2::diag;
    CHECK(exceptionName(0xC0000005) == "access violation" && exceptionName(0xC00000FD) == "stack overflow");
    CHECK(exceptionName(0x123) == "exception 0x123");
    CHECK(exceptionName(0xE0000005) == "closing took too long");
    CHECK(fileName("D:\\Profiles\\Me\\neots2.exe") == "neots2.exe" && fileName("a/b/c.cpp:3") == "c.cpp:3" && fileName("x") == "x");
    // Message text is broken between words, keeping its own breaks. A long path keeps a line.
    CHECK(wrapped("Something went wrong inside NeoTS2.", 16) == "Something went\nwrong inside\nNeoTS2.");
    CHECK(wrapped("ab cd\n\nThe report:\n/home/sam/a-very-long-folder x", 10) == "ab cd\n\nThe\nreport:\n/home/sam/a-very-long-folder\nx");  // a made-up user, leakcheck: ok
    CHECK(wrapped("\xC3\xA9\xC3\xA9\xC3\xA9 abc", 7) == "\xC3\xA9\xC3\xA9\xC3\xA9 abc" && wrapped("", 10).empty());
    CHECK(linkEncoded("a b&c/\xC3\xA9") == "a%20b%26c%2F%C3%A9");
    Crash crash;
    crash.version = "abc1234";
    crash.kind = exceptionName(0xC0000005);
    crash.code = 0xC0000005;
    crash.where = {"neots2.exe", 0x4a955, "app::crash::test", "crash_handler.cpp:318"};
    for (int i = 0; i < 40; ++i) {
        crash.stack.push_back({"D:\\Profiles\\Me\\Games\\NeoTS2\\neots2.exe", 0x1000u + i, "app::f" + std::to_string(i),
                               "D:\\Profiles\\Me\\src\\file.cpp:" + std::to_string(i)});
    }
    const std::string text = crashText(crash, {"12:00:00.000 step: lot opened"});
    CHECK(text.find("Version: abc1234") != std::string::npos && text.find("access violation (0xc0000005)") != std::string::npos);
    CHECK(text.find("neots2.exe+0x4a955  app::crash::test  (crash_handler.cpp:318)") != std::string::npos);
    CHECK(text.find("step: lot opened") != std::string::npos && text.find("Nothing is sent anywhere") != std::string::npos);
    const std::string link = issueLink(crash);
    CHECK(link.rfind(std::string(kIssuesPage) + "?title=", 0) == 0 && link.size() <= 2000);
    CHECK(link.find("abc1234") != std::string::npos && link.find("Users") == std::string::npos && link.find("lot%20opened") == std::string::npos);
}

// A run that ended without a word is told from what Windows noted in its event log: the fields of
// its "Application Error" event, and the crash they amount to.
void unexpectedExitsSayWhatWindowsNoted() {
    using namespace ts2::diag;
    const auto fields = eventFields("<EventData><Data Name='AppName'>neots2.exe</Data><Data Name='ExceptionCode'>c0000409</Data>"
                                    "<Data Name=\"FaultingOffset\">000000000004c172</Data><Data Name='PackageFullName'></Data>"
                                    "<Data Name='Empty'/><Data Name='ModuleName'>neots2.exe</Data></EventData>");
    CHECK(fields.size() == 5 && fields.at("AppName") == "neots2.exe" && fields.at("FaultingOffset") == "000000000004c172");
    CHECK(fields.at("PackageFullName").empty() && !fields.count("Empty"));
    CHECK(eventFields("<Data Name='broken").empty() && eventFields("").empty());

    UnexpectedExit exit;
    exit.version = "abc1234";
    exit.started = "2026-10-12 21:12:10";
    const Crash none = crashOf(exit);
    CHECK(none.kind == "closed unexpectedly" && none.code == 0 && none.version == "abc1234");
    CHECK(unexpectedExitText(exit, {"21:12:10.142 step: lot opened"}).find("Windows noted no crash") != std::string::npos);
    exit.windows = fields;
    const Crash noted = crashOf(exit);
    CHECK(noted.code == 0xC0000409 && noted.where.module == "neots2.exe" && noted.where.offset == 0x4c172);
    CHECK(noted.kind == exceptionName(0xC0000409));
    const std::string text = unexpectedExitText(exit, {"21:12:10.142 step: lot opened"});
    CHECK(text.find("Where: neots2.exe+0x4c172") != std::string::npos && text.find("step: lot opened") != std::string::npos);
    CHECK(text.find("Nothing is sent anywhere") != std::string::npos);
    exit.windows["ExceptionCode"] = "not hex";
    CHECK(crashOf(exit).code == 0);  // nonsense is left out, not thrown
    CHECK(issueLink(noted).find("Users") == std::string::npos);
    // A crash the crash catcher caught (Linux and macOS) says it crashed, with its dump beside it.
    UnexpectedExit caught;
    caught.version = "abc1234";
    caught.started = "2026-10-12 21:12:10";
    caught.caught = true;
    const std::string caughtText = unexpectedExitText(caught, {"21:12:10.142 step: lot opened"});
    CHECK(crashOf(caught).kind == "crash" && caughtText.rfind("NeoTS2 had to close", 0) == 0);
    CHECK(caughtText.find("Version: abc1234") != std::string::npos && caughtText.find("step: lot opened") != std::string::npos);
}

// The pieces of sending a report: a stored zip (checked against the standard CRC and its own
// directory), the upload link read from BugSplat's JSON, and the form the upload is committed with.
void reportUploadPieces() {
    using namespace ts2::diag;
    const std::string check = "123456789";
    CHECK(crc32(reinterpret_cast<const std::uint8_t*>(check.data()), check.size()) == 0xCBF43926u);
    const auto z = zip({{"neots2.dmp", {1, 2, 3}}, {"crash.txt", {'h', 'i'}}});
    auto u32 = [&](std::size_t at) { return z[at] | z[at + 1] << 8 | z[at + 2] << 16 | static_cast<std::uint32_t>(z[at + 3]) << 24; };
    CHECK(z.size() > 22 && u32(0) == 0x04034B50u && u32(z.size() - 22) == 0x06054B50u);
    CHECK(z[z.size() - 22 + 10] == 2);  // two files
    CHECK(std::string(z.begin() + 30, z.begin() + 40) == "neots2.dmp" && z[40] == 1 && z[42] == 3);
    CHECK(zip({}).empty());
    std::ofstream(std::filesystem::temp_directory_path() / "neots2-zip-test.zip", std::ios::binary)
        .write(reinterpret_cast<const char*>(z.data()), static_cast<std::streamsize>(z.size()));

    // As JSON writes a link: slashes escaped, & as &.
    const std::string reply = "{\"status\":\"success\",\"url\":\"https:\\/\\/s3.example.com\\/a?x=1\\u0026y=2\"}";
    CHECK(jsonString(reply, "url") == "https://s3.example.com/a?x=1&y=2");
    CHECK(jsonString(R"({"note":"url", "url" : "b"})", "url") == "b");  // the key's text in a value isn't the key
    CHECK(jsonString(R"({"url":12})", "url").empty() && jsonString(R"({"url":"cut)", "url").empty() && jsonString("", "url").empty());

    const std::string form = formBody({{"appName", "NeoTS2"}, {"md5", "abc"}}, "XYZ");
    CHECK(form == "--XYZ\r\nContent-Disposition: form-data; name=\"appName\"\r\n\r\nNeoTS2\r\n"
                  "--XYZ\r\nContent-Disposition: form-data; name=\"md5\"\r\n\r\nabc\r\n--XYZ--\r\n");
    // Files come after the fields, as bytes (a zero byte included).
    const std::string withFile = formBody({{"product", "NeoTS2"}}, {{"upload_file_minidump", "neots2.dmp", {'M', 0, 'D'}}}, "XYZ");
    CHECK(withFile == std::string("--XYZ\r\nContent-Disposition: form-data; name=\"product\"\r\n\r\nNeoTS2\r\n"
                                  "--XYZ\r\nContent-Disposition: form-data; name=\"upload_file_minidump\"; filename=\"neots2.dmp\"\r\n"
                                  "Content-Type: application/octet-stream\r\n\r\nM") +
                          '\0' + "D\r\n--XYZ--\r\n");
}

// Reports write the user's own folder as %USERPROFILE%: any case, either slash, whole folder names
// only, everywhere it appears.
void reportsDontNameTheUser() {
    using ts2::diag::withoutUserFolder;
    const std::string user = "D:\\Profiles\\Sam";
    CHECK(withoutUserFolder("logging to D:\\Profiles\\Sam\\Documents\\NeoTS2\\logs\\a.log", user) ==
          "logging to %USERPROFILE%\\Documents\\NeoTS2\\logs\\a.log");
    CHECK(withoutUserFolder("d:/profiles/sam/Desktop and \"D:\\Profiles\\Sam\"", user) == "%USERPROFILE%/Desktop and \"%USERPROFILE%\"");
    CHECK(withoutUserFolder("D:\\Profiles\\Sam2\\x and D:\\Profiles\\Sam.old", user) == "D:\\Profiles\\Sam2\\x and D:\\Profiles\\Sam.old");
    CHECK(withoutUserFolder("D:\\Profiles\\Sam\\", "D:\\Profiles\\Sam\\") == "%USERPROFILE%\\");  // given with a slash at its end
    CHECK(withoutUserFolder("C:\\Games", "C:") == "C:\\Games" && withoutUserFolder("text", "") == "text");

    // Their name is left out where it is a whole folder name in a path, anywhere.
    const ts2::diag::User sam{user, {"Sam"}};
    CHECK(ts2::diag::withoutUser("E:\\sam\\NeoTS2 and /mnt/Sam and D:\\Profiles\\Sam\\x", sam) ==
          "E:\\%USERNAME%\\NeoTS2 and /mnt/%USERNAME% and %USERPROFILE%\\x");
    CHECK(ts2::diag::withoutUser("Sam's lot, E:\\Samantha\\x and E:\\Games\\Sam2", sam) == "Sam's lot, E:\\Samantha\\x and E:\\Games\\Sam2");

    // In a dump, the name becomes underscores in 8-bit and UTF-16 paths, keeping every length.
    std::vector<std::uint8_t> dump;
    for (const char c : std::string_view("E:\\SAM\\neots2.exe|Sam|E:\\Samantha")) {
        dump.push_back(static_cast<std::uint8_t>(c));
    }
    for (const char c : std::string_view("C:/sam/x")) {
        dump.push_back(static_cast<std::uint8_t>(c));
        dump.push_back(0);
    }
    const std::size_t size = dump.size();
    CHECK(ts2::diag::maskUser(dump, sam) && dump.size() == size);
    CHECK(std::string(dump.begin(), dump.begin() + 33) == "E:\\___\\neots2.exe|Sam|E:\\Samantha");
    CHECK(dump[33 + 6] == '_' && dump[33 + 7] == 0 && dump[33 + 10] == '_' && dump[33 + 12] == '/');
    CHECK(!ts2::diag::maskUser(dump, sam) && !ts2::diag::maskUser(dump, ts2::diag::User{}));

    // A dump file is rewritten in place at its own size, and one over the limit is left alone.
    namespace fs = std::filesystem;
    const fs::path file = fs::temp_directory_path() / ("neots2-mask-test-" + std::to_string(std::random_device{}()) + ".dmp");
    std::ofstream(file, std::ios::binary) << "D:\\Sam\\a" << std::string(100, '\0');
    CHECK(!ts2::diag::maskUserInFile(file, sam, 10) && ts2::diag::maskUserInFile(file, sam, 1000));
    std::string back;
    {
        std::ifstream in(file, std::ios::binary);
        back.assign(std::istreambuf_iterator<char>(in), {});
    }
    CHECK(back.size() == 108 && back.substr(0, 8) == "D:\\___\\a");
    std::error_code ec;
    fs::remove(file, ec);
}

// A run's log file stops growing at its limit, with a line saying so. The recent lines go on.
void logFileStopsAtItsLimit() {
    namespace fs = std::filesystem;
    ts2::diag::reset();
    const fs::path folder = fs::temp_directory_path() / ("neots2-logcap-test-" + std::to_string(std::random_device{}()));
    CHECK(ts2::diag::open(folder, "header", 300));
    for (int i = 0; i < 40; ++i) {
        ts2::diag::info("line " + std::to_string(i));
    }
    const fs::path file = ts2::diag::file();
    std::string text;
    {
        std::ifstream in(file);
        text.assign(std::istreambuf_iterator<char>(in), {});
    }
    CHECK(text.find("(the log is full") != std::string::npos && text.find("line 39") == std::string::npos);
    CHECK(text.size() < 300 + 80);
    const auto recent = ts2::diag::recentLines();
    CHECK(!recent.empty() && recent.back().find("line 39") != std::string::npos);
    ts2::diag::reset();
    std::error_code ec;
    fs::remove_all(folder, ec);
}

// A warn-once line is written the first time its calling line is reached, with the file name only.
void warnOnceWritesOncePerLine() {
    ts2::diag::reset();
    for (int i = 0; i < 3; ++i) {
        ts2::diag::warnOnce("limit hit");
    }
    ts2::diag::warnOnce("other limit");
    const auto recent = ts2::diag::recentLines();
    CHECK(recent.size() == 2);
    CHECK(recent.size() == 2 && recent[0].find("limit hit (ui_and_diag_tests.cpp:") != std::string::npos);
    CHECK(recent.size() == 2 && recent[1].find("other limit (ui_and_diag_tests.cpp:") != std::string::npos);
    ts2::diag::reset();
}

// Timers count only while timings are on. A report takes the totals and starts over.
void timersCountWhileOn() {
    ts2::diag::setTimings(false);
    { ts2::diag::Timer t("test part"); }
    CHECK(ts2::diag::takeTimings().empty());
    ts2::diag::setTimings(true);
    { ts2::diag::Timer t("test part"); }
    { ts2::diag::Timer t("test part"); }
    const std::string report = ts2::diag::takeTimings();
    CHECK(report.find("test part: 2 calls") != std::string::npos);
    CHECK(ts2::diag::takeTimings().empty());
    ts2::diag::setTimings(false);
}

// Only names the program makes are cleaned up or read. Paths become UTF-8 text whatever the
// Windows language. Loads left running by a household change still count toward the limit.
void diagnosticsKeepToTheirOwnFiles() {
    using namespace ts2::diag;
    CHECK(isLogName("neots2-20261012-153000.log") && !isLogName("neots2-notes.log") && !isLogName("neots2-20261012-153000.log.bak"));
    CHECK(isReportFolderName("crash-20261012-153000") && isReportFolderName("crash-20261012-153000-unexpected-1234"));
    CHECK(!isReportFolderName("crash-course") && !isReportFolderName("crash-20261012-153000-unexpected-") &&
          !isReportFolderName("crash-20261012-153000-old") && !isReportFolderName("Crash-20261012-153000"));
    CHECK(isMarkerName("running-1234.txt") && !isMarkerName("running-.txt") && !isMarkerName("running-12ab.txt") &&
          !isMarkerName("running-1234.txt.old"));
    CHECK(utf8(std::filesystem::path(L"D:\\Profiles\\Jos\u00e9\\\u4e2d\u6587")) == "D:\\Profiles\\Jos\xC3\xA9\\\xE4\xB8\xAD\xE6\x96\x87");

    std::promise<void> open;
    const std::shared_future<void> gate = open.get_future().share();
    ts2::content::TargetPictures pictures([gate](const std::filesystem::path&, std::uint32_t) -> std::optional<ts2::tga::Pixels> {
        gate.wait();
        return std::nullopt;
    });
    for (std::int16_t n = 1; n <= 4; ++n) {
        CHECK(pictures.load(n, "slow", 0x10));
    }
    pictures.clear();  // a new household while all four still load
    CHECK(!pictures.load(5, "slow", 0x10));
    open.set_value();
    bool loaded = false;
    for (int tries = 0; tries < 500 && !loaded; ++tries) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        loaded = pictures.load(5, "slow", 0x10);  // room again once the abandoned loads end
    }
    CHECK(loaded);
    for (int tries = 0; tries < 500 && pictures.loading() > 0; ++tries) {
        pictures.collect();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// The game's font engine measures a Type 1 font from its ascender and descender in 16.16 fixed
// point: ITC Benguiat Gothic bold ('f' 714 high, 'g' 230 deep) at 12 pixels has its baseline 9
// below the line's top and lines 12 high, as read from the running game.
void typeOneLinesAsTheGameMeasures() {
    const auto twelve = ts2::ui::typeOneLine(714, -230, 1000, 12);
    CHECK(twelve.ascent == 9 && twelve.height == 12);
    const auto sixteen = ts2::ui::typeOneLine(714, -230, 1000, 16);
    CHECK(sixteen.ascent == 12 && sixteen.height == 16);
    // Lines past 28 high grow by one more.
    CHECK(ts2::ui::typeOneLine(714, -230, 1000, 28).height == 28);
    CHECK(ts2::ui::typeOneLine(714, -230, 1000, 29).height == 30);
    // Broken numbers stay in range.
    const auto broken = ts2::ui::typeOneLine(-40000, 40000, -5, 100000);
    CHECK(broken.height >= 1 && broken.height <= 4098 && broken.ascent >= -4096 && broken.ascent <= 4096);
}

} // namespace

// The drag arrow keeps its picture unturned, and a quarter turn moves column 5, row 2 to column 2,
// row 26. Dark pixels stay transparent.
void dragArrowTurnsAboutItsMiddle() {
    std::vector<std::uint8_t> grey(32 * 32, 0);
    grey[2 * 32 + 5] = 200;
    grey[3 * 32 + 5] = 20;
    const auto still = ts2::ui::turnDragArrow(grey, 0.0f);
    CHECK(still.width == 32 && still.rgba[(2 * 32 + 5) * 4] == 200 && still.rgba[(2 * 32 + 5) * 4 + 3] == 255);
    CHECK(still.rgba[(3 * 32 + 5) * 4 + 3] == 0);
    const auto turned = ts2::ui::turnDragArrow(grey, 1.5707963f);
    CHECK(turned.rgba[(26 * 32 + 2) * 4] == 200 && turned.rgba[(26 * 32 + 2) * 4 + 3] == 255);
    CHECK(turned.rgba[(2 * 32 + 5) * 4 + 3] == 0);
}

// A want icon's glow and spin as the game played them: the glow rose a frame every 33 ms from 33 ms,
// held its last frame from 462 to about 1090 ms, fell back to its first frame by 1545 ms and ended at
// 1914 ms. The spin turned a frame every 33 ms for 1.5 s from a random frame.
void animatedBitmapsPlayAsTheGame() {
    const std::vector<std::string> glow{"kType=kAlphaFade", "kFrameRate=30", "kFrameCount=15", "kLoopCount=1", "kPingPong=true",
                                        "kLastFrameHold=600", "kFirstFrameHold=800", "kHideWhenDone=true"};
    ts2::ui::AnimatedBitmap a(glow, 40, 40);
    CHECK(a.play(0) && a.type() == ts2::ui::AnimatedBitmap::Type::AlphaFade && a.frames() == 15 && a.hideWhenDone());
    CHECK(a.at(0).frame == 0 && a.at(32).frame == 0 && a.at(33).frame == 1 && a.at(483).frame == 14);
    CHECK(a.at(1088).frame == 14 && a.at(1092).frame == 13 && a.at(1545).frame == 0);
    CHECK(!a.at(1913).done && a.at(1914).done && a.at(1914).frame == 0);
    CHECK(a.alpha(0) == 0 && a.alpha(7) == 127 && a.alpha(14) == 255);
    const std::vector<std::string> spin{"kFrameRate=30", "kDuration=1500", "kRandomStartFrame=true",
                                        "kAnimationSound=ui_wf_slotmachine_lp", "kEndSound=ui_wf_slotmachine_stop"};
    ts2::ui::AnimatedBitmap b(spin, 800, 40);
    CHECK(b.play(27) && b.frames() == 20 && b.at(0).frame == 7 && b.at(33).frame == 8 && b.at(429).frame == 0);
    CHECK(!b.at(1499).done && b.at(1500).done && b.at(1500).frame == 12);
    ts2::ui::AnimatedBitmap still({}, 40, 40);
    CHECK(!still.play(0));
    // The menu's plumbob has no table: 31 frames of its window's width, 30 a second, over and over.
    ts2::ui::AnimatedBitmap plumbob({}, 2635, 85);
    CHECK(plumbob.play(0) && plumbob.frames() == 31 && plumbob.at(32).frame == 0 && plumbob.at(33).frame == 1);
    CHECK(plumbob.at(30 * 33).frame == 30 && plumbob.at(31 * 33).frame == 0 && !plumbob.at(100000).done);
}

// The game's scroll bar as the cheat window's output had it in the game: 44 lines with 13 shown on
// a bar 171 high, scrolled to the end, held its thumb at 139 with 31 positions and 4 pages.
void scrollBarStepsAsTheGame() {
    using Part = ts2::ui::ScrollBar::Part;
    ts2::ui::ScrollBar bar;
    bar.setHeight(171);
    bar.setLines(44, 13);
    CHECK(bar.lastPosition() == 31 && bar.position() == 0 && bar.thumbTop() == 16);
    CHECK(bar.setPosition(31) && bar.thumbTop() == 139);
    CHECK(bar.partAt(5) == Part::Up && bar.partAt(100) == Part::PageUp && bar.partAt(145) == Part::Thumb);
    CHECK(bar.partAt(160) == Part::Down);
    // An arrow moves a line at once and again each time more than 200 ms pass while it is held.
    CHECK(bar.press(5, 1000) && bar.position() == 30 && bar.pressed() == Part::Up);
    CHECK(!bar.tick(1200) && bar.tick(1201) && bar.position() == 29);
    // Off the arrow the press stops repeating.
    bar.move(100, true);
    CHECK(!bar.tick(1500) && bar.position() == 29);
    bar.release(100);
    CHECK(bar.pressed() == Part::None);
    // A page is the 32 positions over 4 pages.
    CHECK(bar.press(20, 2000) && bar.position() == 21);
    bar.release(20);
    // The thumb's middle follows the mouse: at 16 + 8 it is at the top, past the end at the bottom.
    CHECK(!bar.press(bar.thumbTop() + 3, 3000) && bar.pressed() == Part::Thumb);
    bar.move(24, true);
    CHECK(bar.position() == 0 && bar.thumbTop() == 16);
    bar.move(400, false);
    CHECK(bar.position() == 31 && bar.thumbTop() == 139);
    bar.release(78);
    CHECK(bar.position() == 14 && bar.pressed() == Part::None);
    // Nothing to scroll: the thumb stays at the top and the arrows do nothing.
    bar.setLines(5, 13);
    CHECK(bar.lastPosition() == 0 && bar.position() == 0 && bar.thumbTop() == 16 && !bar.press(5, 4000));
}

std::vector<Test> uiAndDiagTests() {
    return {
        {"the drag arrow turns about its middle", dragArrowTurnsAboutItsMiddle},
        {"animated bitmaps play as the game's", animatedBitmapsPlayAsTheGame},
        {"the scroll bar steps as the game's", scrollBarStepsAsTheGame},
        {"pie items go round the centre", pieItemsGoRoundTheCentre},
        {"clicks are dropped while loading", clicksAreDroppedWhileLoading},
        {"pie pages hold eleven and More", piePagesHoldElevenAndMore},
        {"tips are placed as the game", tipsArePlacedAsTheGame},
        {"type 1 lines as the game measures", typeOneLinesAsTheGameMeasures},
        {"ui scripts parse", uiScriptsParse},
        {"log keeps its lines and files", logKeepsItsLinesAndFiles},
        {"crash reports say what happened", crashReportsSayWhatHappened},
        {"unexpected exits say what Windows noted", unexpectedExitsSayWhatWindowsNoted},
        {"report upload pieces", reportUploadPieces},
        {"reports don't name the user", reportsDontNameTheUser},
        {"log file stops at its limit", logFileStopsAtItsLimit},
        {"warn once writes once per line", warnOnceWritesOncePerLine},
        {"timers count while on", timersCountWhileOn},
        {"diagnostics keep to their own files", diagnosticsKeepToTheirOwnFiles},
    };
}
