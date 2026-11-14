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

} // namespace

std::vector<Test> storyAlbumTests() {
    return {
        {"a story album is read", albumIsRead},
        {"an empty or broken story album has no entries", emptyAlbumIsRead},
        {"a story album's file is named by its id", albumFileIsNamedByItsId},
    };
}
