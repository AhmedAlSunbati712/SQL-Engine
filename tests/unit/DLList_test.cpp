#include <gtest/gtest.h>

#include <DLList.h>
#include <Page.h>

namespace {
    TEST(DLListTest, StartsEmpty) {
        DLList list;
        Page *page = list.get(4);
        EXPECT_EQ(page, nullptr);
        EXPECT_EQ(list.len(), 0);
        EXPECT_EQ(list.exists(4), false);
    }
    TEST(DLListTest, AddSinglePage) {
        DLList list;
        Page *page = new Page();
        page->page_num = 4;
        list.add(page->page_num, page);
        EXPECT_EQ(list.len(), 1);
        EXPECT_EQ(list.exists(4), true);
        EXPECT_EQ(list.get(4), page);

        delete page;
    }

    TEST(DLListTest, RemoveSinglePage) {
        DLList list;
        Page *page = new Page();
        page->page_num = 4;
        list.add(page->page_num, page);
        Page *removed_page = list.remove(4);
        EXPECT_EQ(removed_page, page);
        EXPECT_EQ(list.len(), 0);
        EXPECT_EQ(list.exists(4), false);
        EXPECT_EQ(list.get(4), nullptr);
        delete page;
    }

    TEST(DLListTest, AddMultiplePages) {
        DLList list;

        Page *page_one = new Page();
        page_one->page_num = 1;
        list.add(page_one->page_num, page_one);

        Page *page_two = new Page();
        page_two->page_num = 2;
        list.add(page_two->page_num, page_two);

        Page *page_three = new Page();
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

        Page *page_one = new Page();
        page_one->page_num = 1;
        list.add(page_one->page_num, page_one);

        Page *page_two = new Page();
        page_two->page_num = 2;
        list.add(page_two->page_num, page_two);

        Page *page_three = new Page();
        page_three->page_num = 3;
        list.add(page_three->page_num, page_three);

        Page *removed_page = list.remove(2);
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

        Page *page_one = new Page();
        page_one->page_num = 1;
        list.add(page_one->page_num, page_one);

        Page *page_two = new Page();
        page_two->page_num = 2;
        list.add(page_two->page_num, page_two);

        Page *removed_page = list.remove(9);
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

        Page *page_one = new Page();
        page_one->page_num = 1;
        list.add(page_one->page_num, page_one);
        EXPECT_EQ(list.len(), 1);

        Page *page_two = new Page();
        page_two->page_num = 2;
        list.add(page_two->page_num, page_two);
        EXPECT_EQ(list.len(), 2);

        Page *page_three = new Page();
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

        Page *page_one = new Page();
        page_one->page_num = 4;
        list.add(page_one->page_num, page_one);

        Page *removed_page = list.remove(4);
        EXPECT_EQ(removed_page, page_one);
        EXPECT_EQ(list.len(), 0);
        EXPECT_EQ(list.get(4), nullptr);
        delete removed_page;

        Page *page_two = new Page();
        page_two->page_num = 4;
        list.add(page_two->page_num, page_two);

        EXPECT_EQ(list.len(), 1);
        EXPECT_EQ(list.exists(4), true);
        EXPECT_EQ(list.get(4), page_two);

        delete list.remove(4);
    }
}
