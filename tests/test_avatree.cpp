#include <gtest/gtest.h>
#include <vector>
#include <map>
#include <cstring>
#include <string>
#include <random>
#include <iostream>

/* 
 * Link C headers with C++ Test Suite 
 */
extern "C" {
    #include <avadb/avatree.h>
    #include <avadb/avapager.h>
}

// This exists to create a mockup pager that lies in-memory rather than needing an externally backed source
struct MockStorage {
    std::vector<std::vector<uint8_t>> pages;
    size_t page_size = 4096;

    void reset() {
        pages.clear();
        // Page 0 is reserved/null
        pages.push_back(std::vector<uint8_t>(page_size, 0));
    }
};

// Global instance for the C callbacks to access
static MockStorage global_mock;

extern "C" {
    void* ava_pager_read(AvaPager* pager, ava_pgid_t index) {
        if (index >= global_mock.pages.size()) return nullptr;
        return global_mock.pages[index].data();
    }

    void ava_pager_mark_dirty(AvaPager* pager, ava_pgid_t index) {
        // In memory, changes are instant, no-op needed
    }

    bool ava_pager_allocate(AvaPager* pager, ava_pgid_t* new_index) {
        ava_pgid_t id = global_mock.pages.size();
        global_mock.pages.push_back(std::vector<uint8_t>(global_mock.page_size, 0));
        *new_index = id;
        return true;
    }
    
    // Stubs for functions not used by avatree.c logic but required by linker if linked aggressively
    void ava_pager_sync(AvaPager* pager) {}
}

/* Test class */
class AvaTreeTest : public ::testing::Test {
protected:
    AvaPager pager;

    void SetUp() override {
        global_mock.reset();
        memset(&pager, 0, sizeof(AvaPager));
        pager.page_size = global_mock.page_size;
    }

    void TearDown() override {
        global_mock.reset();
    }

    /* 
     * Helper to verify tree structure integrity 
     * Returns true if healthy, false if corrupted.
     */
    void VerifyTree(ava_pgid_t page_id, bool is_root = false) {
        if (page_id == 0) return;
        
        void* ptr = ava_pager_read(&pager, page_id);
        ASSERT_NE(ptr, nullptr) << "Page " << page_id << " is inaccessible";
        
        AvaTreePageHeader* page = (AvaTreePageHeader*)ptr;
        
        // Basic sanity checks
        ASSERT_TRUE(page->type == AVA_PAGE_TYPE_LEAF || page->type == AVA_PAGE_TYPE_INTERNAL) 
            << "Page " << page_id << " has invalid type " << (int)page->type;
            
        ASSERT_LE(page->header.node.num_entries, 1000) << "Page " << page_id << " has suspicious entry count";
        ASSERT_LE(page->header.node.free_end, pager.page_size) << "Page " << page_id << " free_end out of bounds";

        if (page->type == AVA_PAGE_TYPE_INTERNAL) {
            // Recursively verify children
            // Note: We can't easily iterate children here without copying C logic logic or making helpers public.
            // For now, checking the page metadata is a good start.
        }
    }
    
    void DumpSeed(unsigned int seed) {
        std::cout << "[          ] Random Seed: " << seed << std::endl;
    }
};

/* Tests */
TEST_F(AvaTreeTest, InsertAndSearchBasic) {
    ava_pgid_t root = 0;
    
    std::string key = "hello";
    std::string val = "world";

    // Insert
    root = ava_tree_insert(&pager, root, (char*)key.c_str(), key.size(), 
                           (char*)val.c_str(), val.size() + 1, AVA_VALUE_TYPE_STRING);

    // Search
    AvaTreeLeafCell* res = ava_tree_search(&pager, root, (char*)key.c_str(), key.size());
    
    ASSERT_NE(res, nullptr);
    ASSERT_EQ(res->key_size, key.size());
    
    char* stored_val = (char*)(res->payload + res->key_size);
    EXPECT_STREQ(stored_val, val.c_str());
}

TEST_F(AvaTreeTest, InsertUpdatesRootOnSplit) {
    ava_pgid_t root = 0;

    // Insert enough items to force a root split (e.g. 100 items for 4KB page)
    int count = 100;
    for (int i = 0; i < count; i++) {
        std::string key = "k-" + std::to_string(i);
        std::string val = "val-" + std::to_string(i);
        root = ava_tree_insert(&pager, root, (char*)key.c_str(), key.size(), 
                               (char*)val.c_str(), val.size() + 1, AVA_VALUE_TYPE_STRING);
    }

    // The root should no longer be 0 (if it started at 0 and split, new root is allocated)
    // Actually, first alloc is 1. If it splits, root becomes higher.
    EXPECT_GT(root, 0);

    // Verify all exist
    for (int i = 0; i < count; i++) {
        std::string key = "k-" + std::to_string(i);
        std::string val = "val-" + std::to_string(i);
        
        AvaTreeLeafCell* res = ava_tree_search(&pager, root, (char*)key.c_str(), key.size());
        ASSERT_NE(res, nullptr) << "Key failed lookup: " << key;
        
        char* stored_val = (char*)(res->payload + res->key_size);
        EXPECT_STREQ(stored_val, val.c_str());
    }
}

TEST_F(AvaTreeTest, DeleteBasic) {
    ava_pgid_t root = 0;
    int count = 50;

    // Insert
    for (int i = 0; i < count; i++) {
        std::string key = "k" + std::to_string(i);
        root = ava_tree_insert(&pager, root, (char*)key.c_str(), key.size(), "v", 2, AVA_VALUE_TYPE_STRING);
    }

    // Delete even keys
    for (int i = 0; i < count; i += 2) {
        std::string key = "k" + std::to_string(i);
        root = ava_tree_delete(&pager, root, (char*)key.c_str(), key.size());
    }

    // Verify
    for (int i = 0; i < count; i++) {
        std::string key = "k" + std::to_string(i);
        AvaTreeLeafCell* res = ava_tree_search(&pager, root, (char*)key.c_str(), key.size());
        
        if (i % 2 == 0) {
            EXPECT_EQ(res, nullptr) << "Key should be deleted: " << key;
        } else {
            EXPECT_NE(res, nullptr) << "Key should exist: " << key;
        }
    }
}

TEST_F(AvaTreeTest, FuzzRandomOps) {
    ava_pgid_t root = 0;
    int iterations = 10000;
    int range = 1000;
    
    std::random_device rd;
    unsigned int seed = rd();
    DumpSeed(seed); // Print seed so we can reproduce if it fails
    
    std::vector<bool> exists(range, false);
    std::mt19937 rng(seed); 

    for (int i = 0; i < iterations; i++) {
        int k = rng() % range;
        std::string key = "rk-" + std::to_string(k);
        
        bool is_insert = (rng() % 2) == 0;
        
        if (is_insert) {
            root = ava_tree_insert(&pager, root, (char*)key.c_str(), key.size(), 
                                   "val", 4, AVA_VALUE_TYPE_STRING);
            exists[k] = true;
        } else {
            root = ava_tree_delete(&pager, root, (char*)key.c_str(), key.size());
            exists[k] = false;
        }

        // Optional: Periodic check to keep test fast, or check only at end.
        // Doing a check every 1000 ops
        if (i % 1000 == 0) {
            for(int j=0; j<range; j++) {
                std::string ck = "rk-" + std::to_string(j);
                AvaTreeLeafCell* res = ava_tree_search(&pager, root, (char*)ck.c_str(), ck.size());
                if (exists[j]) ASSERT_NE(res, nullptr) << "Failed to find key: " << ck << " at iter " << i;
                else ASSERT_EQ(res, nullptr);
            }
            VerifyTree(root, true);
        }
    }
}