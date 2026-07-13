#include <gtest/gtest.h>

#include <sstream>

#include "Onvif/pugixml.hpp"

TEST(PugiXmlTest, ParsesAndTraversesDocument) {
    const char *xml =
        "<?xml version='1.0'?><catalog enabled='true'>"
        "<camera id='1'><name>Main &amp; Gate</name><fps>25</fps></camera>"
        "<camera id='2'><name><![CDATA[Warehouse]]></name><fps>30</fps></camera>"
        "<!--note--></catalog>";
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_string(xml, pugi::parse_full);
    ASSERT_TRUE(result) << result.description();

    pugi::xml_node catalog = doc.child("catalog");
    EXPECT_TRUE(catalog.attribute("enabled").as_bool());
    EXPECT_EQ(2U, std::distance(catalog.children("camera").begin(), catalog.children("camera").end()));
    pugi::xml_node first = catalog.child("camera");
    EXPECT_EQ(1, first.attribute("id").as_int());
    EXPECT_STREQ("Main & Gate", first.child_value("name"));
    EXPECT_EQ(25, first.child("fps").text().as_int());
    EXPECT_EQ(2, first.next_sibling("camera").attribute("id").as_int());
}

TEST(PugiXmlTest, BuildsModifiesAndCopiesTree) {
    pugi::xml_document doc;
    pugi::xml_node root = doc.append_child("root");
    root.append_attribute("version").set_value(1);
    pugi::xml_node item = root.append_child("item");
    item.append_attribute("id") = 7;
    item.text().set("value");
    item.prepend_child(pugi::node_comment).set_value("comment");

    pugi::xml_node copied = root.append_copy(item);
    copied.attribute("id") = 8;
    copied.set_name("copy");
    EXPECT_TRUE(root.insert_child_before("before", item));
    EXPECT_TRUE(root.insert_child_after("after", item));
    EXPECT_TRUE(root.remove_child("before"));
    EXPECT_TRUE(root.remove_attribute("version"));

    std::ostringstream out;
    doc.save(out, "  ", pugi::format_indent | pugi::format_no_declaration);
    EXPECT_NE(std::string::npos, out.str().find("<copy id=\"8\">"));
}

TEST(PugiXmlTest, SupportsXpathQueries) {
    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string(
        "<root><item id='1' score='10'>a</item><item id='2' score='20'>b</item>"
        "<group><item id='3' score='30'>c</item></group></root>"));

    pugi::xpath_node_set nodes = doc.select_nodes("//item[@score >= 20]");
    ASSERT_EQ(2U, nodes.size());
    EXPECT_EQ(2, nodes.first().node().attribute("id").as_int());
    pugi::xpath_query sum_query("sum(//item/@score)");
    EXPECT_DOUBLE_EQ(60.0, sum_query.evaluate_number(doc));
    EXPECT_TRUE(doc.select_node("//item[@id='3']"));
    EXPECT_THROW(pugi::xpath_query("//*["), pugi::xpath_exception);
}

TEST(PugiXmlTest, HandlesFilesStreamsAndEncoding) {
    const char *path = "/tmp/s3mediakit_pugi_test.xml";
    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string("<root><value>42</value></root>"));
    ASSERT_TRUE(doc.save_file(path, "\t", pugi::format_default, pugi::encoding_utf8));

    pugi::xml_document loaded;
    ASSERT_TRUE(loaded.load_file(path));
    EXPECT_EQ(42, loaded.child("root").child("value").text().as_int());

    std::istringstream stream("<stream><v>3.5</v></stream>");
    pugi::xml_document streamed;
    ASSERT_TRUE(streamed.load(stream));
    EXPECT_DOUBLE_EQ(3.5, streamed.child("stream").child("v").text().as_double());
    std::remove(path);
}

TEST(PugiXmlTest, ReportsMalformedInputAndDefaults) {
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_string("<root><broken></root>");
    EXPECT_FALSE(result);
    EXPECT_NE(pugi::status_ok, result.status);
    EXPECT_FALSE(doc.child("missing"));
    EXPECT_EQ(123, doc.child("missing").attribute("x").as_int(123));
    EXPECT_STREQ("fallback", doc.child("missing").text().as_string("fallback"));
}
