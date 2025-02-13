// Copyright Vespa.ai. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include <vespa/searchcommon/attribute/config.h>
#include <vespa/searchlib/attribute/attribute.h>
#include <vespa/searchlib/attribute/attribute_read_guard.h>
#include <vespa/searchlib/attribute/attributefactory.h>
#include <vespa/searchlib/attribute/attributeguard.h>
#include <vespa/searchlib/attribute/attributememorysavetarget.h>
#include <vespa/searchlib/attribute/i_docid_with_weight_posting_store.h>
#include <vespa/searchlib/index/dummyfileheadercontext.h>
#include <vespa/searchlib/queryeval/docid_with_weight_search_iterator.h>
#define ENABLE_GTEST_MIGRATION
#include <vespa/searchlib/test/searchiteratorverifier.h>
#include <vespa/searchlib/util/randomgenerator.h>
#include <vespa/vespalib/gtest/gtest.h>
#include <vespa/vespalib/test/insertion_operators.h>

#include <vespa/log/log.h>
LOG_SETUP("direct_posting_store_test");

using namespace search;
using namespace search::attribute;

AttributeVector::SP make_attribute(BasicType type, CollectionType collection, bool fast_search) {
    Config cfg(type, collection);
    cfg.setFastSearch(fast_search);
    return AttributeFactory::createAttribute("my_attribute", cfg);
}

void add_docs(AttributeVector::SP attr_ptr, size_t limit = 1000) {
    AttributeVector::DocId docid;
    for (size_t i = 0; i < limit; ++i) {
        attr_ptr->addDoc(docid);
    }
    attr_ptr->commit();
    ASSERT_EQ((limit - 1), docid);
}

template <typename ATTR, typename KEY>
void set_doc(ATTR *attr, uint32_t docid, KEY key, int32_t weight) {
    attr->clearDoc(docid);
    attr->append(docid, key, weight);
    attr->commit();
}

void populate_long(AttributeVector::SP attr_ptr) {
    IntegerAttribute *attr = static_cast<IntegerAttribute *>(attr_ptr.get());
    set_doc(attr, 1, int64_t(111), 20);
    set_doc(attr, 5, int64_t(111), 5);
    set_doc(attr, 7, int64_t(111), 10);
}

void populate_string(AttributeVector::SP attr_ptr) {
    StringAttribute *attr = static_cast<StringAttribute *>(attr_ptr.get());
    set_doc(attr, 1, "foo", 20);
    set_doc(attr, 5, "foo", 5);
    set_doc(attr, 7, "foo", 10);
}

struct DirectPostingStoreTest : public ::testing::Test {
    AttributeVector::SP attr;
    const IDocidWithWeightPostingStore *api;

    DirectPostingStoreTest(BasicType type, CollectionType col_type)
        : attr(make_attribute(type, col_type, true)),
          api(attr->as_docid_with_weight_posting_store())
    {
        assert(api != nullptr);
        add_docs(attr);
    }
    ~DirectPostingStoreTest() {}
};

struct DirectIntegerTest : public DirectPostingStoreTest {
    DirectIntegerTest() : DirectPostingStoreTest(BasicType::INT64, CollectionType::WSET)
    {
        populate_long(attr);
    }
};

struct DirectStringTest : public DirectPostingStoreTest {
    DirectStringTest() : DirectPostingStoreTest(BasicType::STRING, CollectionType::WSET)
    {
        populate_string(attr);
    }
};

TEST(DirectPostingStoreApiTest, attributes_support_IDocidWithWeightPostingStore_interface) {
    EXPECT_TRUE(make_attribute(BasicType::INT64, CollectionType::WSET, true)->as_docid_with_weight_posting_store() != nullptr);
    EXPECT_TRUE(make_attribute(BasicType::STRING, CollectionType::WSET, true)->as_docid_with_weight_posting_store() != nullptr);
}

TEST(DirectPostingStoreApiTest, attributes_do_not_support_IDocidWithWeightPostingStore_interface) {
    EXPECT_TRUE(make_attribute(BasicType::INT64, CollectionType::SINGLE, false)->as_docid_with_weight_posting_store() == nullptr);
    EXPECT_TRUE(make_attribute(BasicType::INT64, CollectionType::ARRAY, false)->as_docid_with_weight_posting_store() == nullptr);
    EXPECT_TRUE(make_attribute(BasicType::INT64, CollectionType::WSET, false)->as_docid_with_weight_posting_store() == nullptr);
    EXPECT_TRUE(make_attribute(BasicType::INT64, CollectionType::SINGLE, true)->as_docid_with_weight_posting_store() == nullptr);
    EXPECT_TRUE(make_attribute(BasicType::INT64, CollectionType::ARRAY, true)->as_docid_with_weight_posting_store() == nullptr);
    EXPECT_TRUE(make_attribute(BasicType::STRING, CollectionType::SINGLE, false)->as_docid_with_weight_posting_store() == nullptr);
    EXPECT_TRUE(make_attribute(BasicType::STRING, CollectionType::ARRAY, false)->as_docid_with_weight_posting_store() == nullptr);
    EXPECT_TRUE(make_attribute(BasicType::STRING, CollectionType::WSET, false)->as_docid_with_weight_posting_store() == nullptr);
    EXPECT_TRUE(make_attribute(BasicType::STRING, CollectionType::SINGLE, true)->as_docid_with_weight_posting_store() == nullptr);
    EXPECT_TRUE(make_attribute(BasicType::STRING, CollectionType::ARRAY, true)->as_docid_with_weight_posting_store() == nullptr);
    EXPECT_TRUE(make_attribute(BasicType::INT32, CollectionType::WSET, true)->as_docid_with_weight_posting_store() == nullptr);
    EXPECT_TRUE(make_attribute(BasicType::DOUBLE, CollectionType::WSET, true)->as_docid_with_weight_posting_store() == nullptr);
}

void verify_valid_lookup(IDirectPostingStore::LookupResult result) {
    EXPECT_TRUE(result.posting_idx.valid());
    EXPECT_EQ(3u, result.posting_size);
    EXPECT_EQ(5, result.min_weight);
    EXPECT_EQ(20, result.max_weight);
}

void verify_invalid_lookup(IDirectPostingStore::LookupResult result) {
    EXPECT_FALSE(result.posting_idx.valid());
    EXPECT_EQ(0u, result.posting_size);
    EXPECT_EQ(0, result.min_weight);
    EXPECT_EQ(0, result.max_weight);
}

TEST_F(DirectIntegerTest, lookup_works_correctly) {
    verify_valid_lookup(api->lookup("111", api->get_dictionary_snapshot()));
    verify_invalid_lookup(api->lookup("222", api->get_dictionary_snapshot()));
}

TEST_F(DirectStringTest, lookup_works_correctly) {
    verify_valid_lookup(api->lookup("foo", api->get_dictionary_snapshot()));
    verify_invalid_lookup(api->lookup("bar", api->get_dictionary_snapshot()));
}

void verify_posting(const IDocidWithWeightPostingStore &api, const char *term) {
    auto result = api.lookup(term, api.get_dictionary_snapshot());
    ASSERT_TRUE(result.posting_idx.valid());
    std::vector<DocidWithWeightIterator> itr_store;
    api.create(result.posting_idx, itr_store);
    ASSERT_EQ(1u, itr_store.size());
    {
        DocidWithWeightIterator &itr = itr_store[0];
        if (itr.valid() && itr.getKey() < 1) {
            itr.linearSeek(1);
        }
        ASSERT_TRUE(itr.valid());
        EXPECT_EQ(1u, itr.getKey());  // docid
        EXPECT_EQ(20, itr.getData()); // weight
        itr.linearSeek(2);
        ASSERT_TRUE(itr.valid());
        EXPECT_EQ(5u, itr.getKey());  // docid
        EXPECT_EQ(5, itr.getData());  // weight
        itr.linearSeek(6);
        ASSERT_TRUE(itr.valid());
        EXPECT_EQ(7u, itr.getKey());  // docid
        EXPECT_EQ(10, itr.getData()); // weight
        itr.linearSeek(8);
        EXPECT_FALSE(itr.valid());
    }
}

TEST_F(DirectIntegerTest, iterators_are_created_correctly) {
    verify_posting(*api, "111");
}

TEST_F(DirectStringTest, iterators_are_created_correctly) {
    verify_posting(*api, "foo");
}

TEST_F(DirectStringTest, collect_folded_works)
{
    auto* sa = static_cast<StringAttribute*>(attr.get());
    set_doc(sa, 2, "bar", 30);
    attr->commit();
    set_doc(sa, 3, "FOO", 30);
    attr->commit();
    auto dictionary_snapshot = api->get_dictionary_snapshot();
    auto lookup1 = api->lookup("foo", dictionary_snapshot);
    std::vector<vespalib::string> folded;
    std::function<void(vespalib::datastore::EntryRef)> save_folded = [&folded,sa](vespalib::datastore::EntryRef enum_idx) { folded.emplace_back(sa->getFromEnum(enum_idx.ref())); };
    api->collect_folded(lookup1.enum_idx, dictionary_snapshot, save_folded);
    std::vector<vespalib::string> expected_folded{"FOO", "foo"};
    EXPECT_EQ(expected_folded, folded);
}

TEST_F(DirectIntegerTest, collect_folded_works)
{
    auto* ia = dynamic_cast<IntegerAttributeTemplate<int64_t>*>(attr.get());
    set_doc(ia, 2, int64_t(112), 30);
    attr->commit();
    auto dictionary_snapshot = api->get_dictionary_snapshot();
    auto lookup1 = api->lookup("111", dictionary_snapshot);
    std::vector<int64_t> folded;
    std::function<void(vespalib::datastore::EntryRef)> save_folded = [&folded,ia](vespalib::datastore::EntryRef enum_idx) { folded.emplace_back(ia->getFromEnum(enum_idx.ref())); };
    api->collect_folded(lookup1.enum_idx, dictionary_snapshot, save_folded);
    std::vector<int64_t> expected_folded{int64_t(111)};
    EXPECT_EQ(expected_folded, folded);
}

class Verifier : public search::test::SearchIteratorVerifier {
public:
    Verifier();
    ~Verifier();
    SearchIterator::UP create(bool strict) const override {
        (void) strict;
        const auto* api = _attr->as_docid_with_weight_posting_store();
        assert(api != nullptr);
        auto dict_entry = api->lookup("123", api->get_dictionary_snapshot());
        assert(dict_entry.posting_idx.valid());
        return std::make_unique<queryeval::DocidWithWeightSearchIterator>(_tfmd, *api, dict_entry);
    }
private:
    mutable fef::TermFieldMatchData _tfmd;
    AttributeVector::SP _attr;
};

Verifier::Verifier()
    : _attr(make_attribute(BasicType::INT64, CollectionType::WSET, true))
{
    add_docs(_attr, getDocIdLimit());
    auto docids = getExpectedDocIds();
    IntegerAttribute *int_attr = static_cast<IntegerAttribute *>(_attr.get());
    for (auto docid: docids) {
        set_doc(int_attr, docid, int64_t(123), 1);
    }
}
Verifier::~Verifier() {}

TEST(VerifierTest, verify_document_weight_search_iterator) {
    Verifier verifier;
    verifier.verify();
}

GTEST_MAIN_RUN_ALL_TESTS()
