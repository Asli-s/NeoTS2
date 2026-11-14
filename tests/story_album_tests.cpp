// Story albums as the game keeps them in the Storytelling folder (webentry_<album>.xml).
#include "test_support.hpp"

#include "content/story_album.hpp"

namespace {

constexpr const char* kAlbum = R"(<?xml version="1.0" encoding="UTF-8"?>
<StoryCreator>
  <ExchangeMetadata>
    <Album>
      <ID>0x00000001</ID>
      <FamilyName><![CDATA[]]></FamilyName>
      <FamilyId>0</FamilyId>
      <NeighborhoodName><![CDATA[Pleasantview]]></NeighborhoodName>
      <Title><![CDATA[]]></Title>
      <Style>Album</Style>
      <Entry>
        <Title><![CDATA[First]]></Title>
        <Snapshot>snapshot_00000001_2de303e8.jpg</Snapshot>
        <Text><![CDATA[]]></Text>
        <TextResourceId>0x00a9003c</TextResourceId>
      </Entry>
      <Entry>
        <Snapshot>snapshot_00000001_2de303eb.jpg</Snapshot>
        <Text><![CDATA[Bella & Mortimer <3]]></Text>
        <TextResourceId>0x00000000</TextResourceId>
      </Entry>
      <Entry>
        <Snapshot>..\..\secret.jpg</Snapshot>
      </Entry>
      <Entry>
        <Snapshot>snapshot_00000001_ade303e5.jpg</Snapshot>
        <Text>Tom &amp; Jerry &lt;3</Text>
      </Entry>
    </Album>
  </ExchangeMetadata>
</StoryCreator>)";

void albumIsRead() {
    const auto album = ts2::content::parseStoryAlbum(kAlbum);
    CHECK(album.neighborhood == "Pleasantview");
    CHECK(album.familyId == 0);
    CHECK(album.style == "Album");
    CHECK(album.entries.size() == 3);  // the one naming a path is left out
    CHECK(album.entries[0].snapshot == "snapshot_00000001_2de303e8.jpg");
    CHECK(album.entries[0].textResource == 0x00a9003c);
    CHECK(album.entries[0].text.empty());
    CHECK(album.entries[1].text == "Bella & Mortimer <3");
    CHECK(album.entries[1].textResource == 0);
    CHECK(album.entries[2].text == "Tom & Jerry <3");
}

void emptyAlbumIsRead() {
    const auto album = ts2::content::parseStoryAlbum("<StoryCreator><Album><Style>Blog</Style></Album></StoryCreator>");
    CHECK(album.style == "Blog");
    CHECK(album.entries.empty());
    CHECK(ts2::content::parseStoryAlbum("").entries.empty());
    CHECK(ts2::content::parseStoryAlbum("<Entry><Snapshot>a.jpg").entries.empty());
}

void albumFileIsNamedByItsId() {
    CHECK(ts2::content::storyAlbumFile("S", 1).filename() == "webentry_00000001.xml");
    CHECK(ts2::content::storyAlbumFile("S", 0x2d7b3372).filename() == "webentry_2d7b3372.xml");
}

// A file as the game writes one (CRLF lines, no line ending after the last).
std::string gameFile() {
    const char* lines[] = {
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>", "<StoryCreator>", "  <ClientEnvironment>", "    <AppVersion>1.0.0.906</AppVersion>",
        "  </ClientEnvironment>", "  <ExchangeMetadata>", "    <Album>", "      <ID>0x00000001</ID>", "      <FamilyName><![CDATA[]]></FamilyName>",
        "      <FamilyId>0</FamilyId>", "      <LinkName></LinkName>", "      <NeighborhoodName><![CDATA[Pleasantview]]></NeighborhoodName>",
        "      <LotID>0x00000000</LotID>", "      <LotName><![CDATA[]]></LotName>", "      <Title><![CDATA[]]></Title>", "      <Style>Album</Style>",
        "      <Entry>", "        <Title><![CDATA[]]></Title>", "        <ThumbnailFilename>thumbnail_00000001_2de303e8.jpg</ThumbnailFilename>",
        "        <Snapshot>snapshot_00000001_2de303e8.jpg</Snapshot>", "        <Text><![CDATA[]]></Text>",
        "        <TextResourceId>0x00a9003c</TextResourceId>", "        <TimeStamp>7/28/2004 0:23:39</TimeStamp>", "      </Entry>",
        "      <Entry>", "        <Title><![CDATA[]]></Title>", "        <ThumbnailFilename>thumbnail_00000001_2de303eb.jpg</ThumbnailFilename>",
        "        <Snapshot>snapshot_00000001_2de303eb.jpg</Snapshot>", "        <Text><![CDATA[Hello & bye]]></Text>",
        "        <TextResourceId>0x00000000</TextResourceId>", "        <TimeStamp>7/28/2004 0:23:40</TimeStamp>", "      </Entry>",
        "      <KeywordList/>", "      <UploadDescription><![CDATA[]]></UploadDescription>", "    </Album>", "  </ExchangeMetadata>", "</StoryCreator>"};
    std::string out;
    for (std::size_t i = 0; i < std::size(lines); ++i) {
        out += lines[i];
        if (i + 1 < std::size(lines)) {
            out += "\r\n";
        }
    }
    return out;
}

void albumIsWrittenAsTheGameWritesIt() {
    const std::string file = gameFile();
    auto album = ts2::content::parseStoryAlbum(file);
    CHECK(album.entries.size() == 2 && album.entries[0].thumbnail == "thumbnail_00000001_2de303e8.jpg" &&
          album.entries[0].timeStamp == "7/28/2004 0:23:39" && album.newline == "\r\n");
    CHECK(ts2::content::writeStoryAlbum(album, 1) == file);  // unchanged, byte for byte
    // A caption, title, style and order changed, and an entry added, read back as written.
    album.title = "Our town ]]> story";
    album.style = "Blog";
    album.entries[1].text = "New <caption>";
    std::swap(album.entries[0], album.entries[1]);
    album.entries.push_back({"", "thumbnail_00000001_00000005.jpg", "snapshot_00000001_00000005.jpg", "Third", 0, "1/2/2026 3:04:05"});
    const auto written = ts2::content::writeStoryAlbum(album, 1);
    const auto again = ts2::content::parseStoryAlbum(written);
    CHECK(again.title == "Our town ]]> story" && again.style == "Blog" && again.neighborhood == "Pleasantview");
    CHECK(again.entries.size() == 3 && again.entries[0].text == "New <caption>" && again.entries[1].textResource == 0x00a9003c &&
          again.entries[2].snapshot == "snapshot_00000001_00000005.jpg" && again.entries[2].timeStamp == "1/2/2026 3:04:05");
    CHECK(written.find("<AppVersion>1.0.0.906</AppVersion>") != std::string::npos && written.ends_with("</StoryCreator>"));
    // Every entry taken out: the keywords and the rest stay.
    album.entries.clear();
    const auto empty = ts2::content::parseStoryAlbum(ts2::content::writeStoryAlbum(album, 1));
    CHECK(empty.entries.empty() && empty.title == "Our town ]]> story");
    CHECK(ts2::content::writeStoryAlbum(empty, 1).find("<KeywordList/>") != std::string::npos);
}

void newAlbumIsWrittenWhole() {
    ts2::content::StoryAlbum album;
    album.neighborhood = "Riverblossom Hills";
    album.entries.push_back({"", "thumbnail_00000005_0000000a.jpg", "snapshot_00000005_0000000a.jpg", "Hi", 0, "1/1/2026 0:00:00"});
    const auto written = ts2::content::writeStoryAlbum(album, 5);
    CHECK(written.starts_with("<?xml") && written.find("<ID>0x00000005</ID>") != std::string::npos && written.ends_with("</StoryCreator>"));
    const auto again = ts2::content::parseStoryAlbum(written);
    CHECK(again.neighborhood == "Riverblossom Hills" && again.entries.size() == 1 && again.entries[0].text == "Hi");
    CHECK(ts2::content::writeStoryAlbum(again, 5) == written);
    CHECK(ts2::content::storyTimeStamp(0).find('/') != std::string::npos);
}

} // namespace

std::vector<Test> storyAlbumTests() {
    return {
        {"a story album is read", albumIsRead},
        {"an empty or broken story album has no entries", emptyAlbumIsRead},
        {"a story album's file is named by its id", albumFileIsNamedByItsId},
        {"a story album is written as the game writes it", albumIsWrittenAsTheGameWritesIt},
        {"a new story album is written whole", newAlbumIsWrittenWhole},
    };
}
