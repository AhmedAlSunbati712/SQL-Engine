#include <gtest/gtest.h>

#include <DLList.h>
#include <PageV2.h>

namespace {
    TEST(DLListTest, StartsEmpty) {
        DLList list;
        PageV2 *page = list.get(4);
        EXPECT_EQ(page, nullptr);
        EXPECT_EQ(list.len(), 0);
        EXPECT_EQ(list.exists(4), false);
    }
    TEST(DLListTest, AddSinglePage) {
        DLList list;
        PageV2 *page = new PageV2();
        page->page_num = 4;
        list.add(page->page_num, page);
        EXPECT_EQ(list.len(), 1);
        EXPECT_EQ(list.exists(4), true);
        EXPECT_EQ(list.get(4), page);

        delete page;
    }

    TEST(DLListTest, RemoveSinglePage) {
        DLList list;
        PageV2 *page = new PageV2();
        page->page_num = 4;
        list.add(page->page_num, page);
        PageV2 *removed_page = list.remove(4);
        EXPECT_EQ(removed_page, page);
        EXPECT_EQ(list.len(), 0);
        EXPECT_EQ(list.exists(4), false);
        EXPECT_EQ(list.get(4), nullptr);
        delete page;
    }

    TEST(DLListTest, AddMultiplePages) {
        DLList list;

        PageV2 *page_one = new PageV2();
        page_one->page_num = 1;
        list.add(page_one->page_num, page_one);

        PageV2 *page_two = new PageV2();
        page_two->page_num = 2;
        list.add(page_two->page_num, page_two);

        PageV2 *page_three = new PageV2();
        page_three->page_num = 3;
        list.add(page_three->page_num, page_three);

        EXPECT_EQ(list.len(), 3);
        EXPECT_EQ(list.get(1), page_one);
        EXPECT_EQ(list.get(2), page_two);
        EXPECT_EQ(list.get(3), page_three);
        EXPECT_EQ(list.exists(1), true);
        EXPECT_EQ(list.exists(2), true);
        EXPECT_EQ(list.exists(3), true);

        delete list.remove(1);
        delete list.remove(2);
        delete list.remove(3);
    }

    TEST(DLListTest, RemoveMiddlePage) {
        DLList list;

        PageV2 *page_one = new PageV2();
        page_one->page_num = 1;
        list.add(page_one->page_num, page_one);

        PageV2 *page_two = new PageV2();
        page_two->page_num = 2;
        list.add(page_two->page_num, page_two);

        PageV2 *page_three = new PageV2();
        page_three->page_num = 3;
        list.add(page_three->page_num, page_three);

        PageV2 *removed_page = list.remove(2);
        EXPECT_EQ(removed_page, page_two);
        EXPECT_EQ(list.len(), 2);
        EXPECT_EQ(list.exists(2), false);
        EXPECT_EQ(list.get(2), nullptr);
        EXPECT_EQ(list.get(1), page_one);
        EXPECT_EQ(list.get(3), page_three);

        delete removed_page;
        delete list.remove(1);
        delete list.remove(3);
    }

    TEST(DLListTest, RemoveNonexistentPage) {
        DLList list;

        PageV2 *page_one = new PageV2();
        page_one->page_num = 1;
        list.add(page_one->page_num, page_one);

        PageV2 *page_two = new PageV2();
        page_two->page_num = 2;
        list.add(page_two->page_num, page_two);

        PageV2 *removed_page = list.remove(9);
        EXPECT_EQ(removed_page, nullptr);
        EXPECT_EQ(list.len(), 2);
        EXPECT_EQ(list.get(1), page_one);
        EXPECT_EQ(list.get(2), page_two);
        EXPECT_EQ(list.exists(1), true);
        EXPECT_EQ(list.exists(2), true);

        delete list.remove(1);
        delete list.remove(2);
    }

    TEST(DLListTest, LengthTracksAddsAndRemoves) {
        DLList list;
        EXPECT_EQ(list.len(), 0);

        PageV2 *page_one = new PageV2();
        page_one->page_num = 1;
        list.add(page_one->page_num, page_one);
        EXPECT_EQ(list.len(), 1);

        PageV2 *page_two = new PageV2();
        page_two->page_num = 2;
        list.add(page_two->page_num, page_two);
        EXPECT_EQ(list.len(), 2);

        PageV2 *page_three = new PageV2();
        page_three->page_num = 3;
        list.add(page_three->page_num, page_three);
        EXPECT_EQ(list.len(), 3);

        delete list.remove(2);
        EXPECT_EQ(list.len(), 2);

        delete list.remove(1);
        EXPECT_EQ(list.len(), 1);

        delete list.remove(3);
        EXPECT_EQ(list.len(), 0);
    }

    TEST(DLListTest, CanReinsertPageNumberAfterRemoval) {
        DLList list;

        PageV2 *page_one = new PageV2();
        page_one->page_num = 4;
        list.add(page_one->page_num, page_one);

        PageV2 *removed_page = list.remove(4);
        EXPECT_EQ(removed_page, page_one);
        EXPECT_EQ(list.len(), 0);
        EXPECT_EQ(list.get(4), nullptr);
        delete removed_page;

        PageV2 *page_two = new PageV2();
        page_two->page_num = 4;
        list.add(page_two->page_num, page_two);

        EXPECT_EQ(list.len(), 1);
        EXPECT_EQ(list.exists(4), true);
        EXPECT_EQ(list.get(4), page_two);

        delete list.remove(4);
    }
}
