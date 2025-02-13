// Copyright Vespa.ai. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "query.h"
#include "nearest_neighbor_query_node.h"
#include <vespa/searchlib/parsequery/stackdumpiterator.h>
#include <vespa/searchlib/query/streaming/dot_product_term.h>
#include <vespa/searchlib/query/streaming/in_term.h>
#include <vespa/searchlib/query/tree/term_vector.h>
#include <charconv>
#include <vespa/log/log.h>
LOG_SETUP(".vsm.querynode");

namespace search::streaming {

namespace {

vespalib::stringref DEFAULT("default");
bool disableRewrite(const QueryNode * qn) {
    return dynamic_cast<const NearQueryNode *> (qn) ||
           dynamic_cast<const PhraseQueryNode *> (qn) ||
           dynamic_cast<const SameElementQueryNode *>(qn);
}

bool possibleFloat(const QueryTerm & qt, const QueryTerm::string & term) {
    return !qt.encoding().isBase10Integer() && qt.encoding().isFloat() && (term.find('.') != QueryTerm::string::npos);
}

}

QueryNode::UP
QueryNode::Build(const QueryNode * parent, const QueryNodeResultFactory & factory,
                 SimpleQueryStackDumpIterator & queryRep, bool allowRewrite)
{
    unsigned int arity = queryRep.getArity();
    ParseItem::ItemType type = queryRep.getType();
    UP qn;
    switch (type) {
    case ParseItem::ITEM_AND:
    case ParseItem::ITEM_OR:
    case ParseItem::ITEM_WEAK_AND:
    case ParseItem::ITEM_EQUIV:
    case ParseItem::ITEM_WEIGHTED_SET:
    case ParseItem::ITEM_WAND:
    case ParseItem::ITEM_NOT:
    case ParseItem::ITEM_PHRASE:
    case ParseItem::ITEM_SAME_ELEMENT:
    case ParseItem::ITEM_NEAR:
    case ParseItem::ITEM_ONEAR:
    {
        qn = QueryConnector::create(type);
        if (qn) {
            auto * qc = dynamic_cast<QueryConnector *> (qn.get());
            auto * nqn = dynamic_cast<NearQueryNode *> (qc);
            if (nqn) {
                nqn->distance(queryRep.getNearDistance());
            }
            if ((type == ParseItem::ITEM_WEAK_AND) ||
                (type == ParseItem::ITEM_WEIGHTED_SET) ||
                (type == ParseItem::ITEM_DOT_PRODUCT) ||
                (type == ParseItem::ITEM_SAME_ELEMENT) ||
                (type == ParseItem::ITEM_WAND))
            {
                qn->setIndex(queryRep.getIndexName());
            }
            for (size_t i=0; i < arity; i++) {
                queryRep.next();
                if (qc->isFlattenable(queryRep.getType())) {
                    arity += queryRep.getArity();
                } else {
                    qc->addChild(Build(qc, factory, queryRep, allowRewrite && !disableRewrite(qn.get())));
                }
            }
        }
    }
    break;
    case ParseItem::ITEM_TRUE:
        qn = std::make_unique<TrueNode>();
        break;
    case ParseItem::ITEM_FALSE:
        qn = std::make_unique<FalseNode>();
        break;
    case ParseItem::ITEM_GEO_LOCATION_TERM:
        // just keep the string representation here; parsed in vsm::GeoPosFieldSearcher
        qn = std::make_unique<QueryTerm>(factory.create(),
                                         queryRep.getTerm(),
                                         queryRep.getIndexName(),
                                         QueryTerm::Type::GEO_LOCATION);
        break;
    case ParseItem::ITEM_NEAREST_NEIGHBOR:
        qn = build_nearest_neighbor_query_node(factory, queryRep);
        break;
    case ParseItem::ITEM_NUMTERM:
    case ParseItem::ITEM_TERM:
    case ParseItem::ITEM_PREFIXTERM:
    case ParseItem::ITEM_REGEXP:
    case ParseItem::ITEM_SUBSTRINGTERM:
    case ParseItem::ITEM_EXACTSTRINGTERM:
    case ParseItem::ITEM_SUFFIXTERM:
    case ParseItem::ITEM_PURE_WEIGHTED_STRING:
    case ParseItem::ITEM_PURE_WEIGHTED_LONG:
    case ParseItem::ITEM_FUZZY:
    {
        vespalib::string index = queryRep.getIndexName();
        if (index.empty()) {
            if ((type == ParseItem::ITEM_PURE_WEIGHTED_STRING) || (type == ParseItem::ITEM_PURE_WEIGHTED_LONG)) {
                index = parent->getIndex();
            } else {
                index = DEFAULT;
            }
        }
        if (dynamic_cast<const SameElementQueryNode *>(parent) != nullptr) {
            index = parent->getIndex() + "." + index;
        }
        using TermType = QueryTerm::Type;
        TermType sTerm(TermType::WORD);
        switch (type) {
        case ParseItem::ITEM_REGEXP:
            sTerm = TermType::REGEXP;
            break;
        case ParseItem::ITEM_PREFIXTERM:
            sTerm = TermType::PREFIXTERM;
            break;
        case ParseItem::ITEM_SUBSTRINGTERM:
            sTerm = TermType::SUBSTRINGTERM;
            break;
        case ParseItem::ITEM_EXACTSTRINGTERM:
            sTerm = TermType::EXACTSTRINGTERM;
            break;
        case ParseItem::ITEM_SUFFIXTERM:
            sTerm = TermType::SUFFIXTERM;
            break;
        case ParseItem::ITEM_FUZZY:
            sTerm = TermType::FUZZYTERM;
            break;
        default:
            break;
        }
        QueryTerm::string ssTerm;
        if (type == ParseItem::ITEM_PURE_WEIGHTED_LONG) {
            char buf[24];
            auto res = std::to_chars(buf, buf + sizeof(buf), queryRep.getIntergerTerm(), 10);
            ssTerm.assign(buf, res.ptr - buf);
        } else {
            ssTerm = queryRep.getTerm();
        }
        QueryTerm::string ssIndex(index);
        if (ssIndex == "sddocname") {
            // This is suboptimal as the term should be checked too.
            // But it will do for now as only correct sddocname queries are sent down.
            qn = std::make_unique<TrueNode>();
        } else {
            auto qt = std::make_unique<QueryTerm>(factory.create(), ssTerm, ssIndex, sTerm);
            qt->setWeight(queryRep.GetWeight());
            qt->setUniqueId(queryRep.getUniqueId());
            if (qt->isFuzzy()) {
                qt->setFuzzyMaxEditDistance(queryRep.getFuzzyMaxEditDistance());
                qt->setFuzzyPrefixLength(queryRep.getFuzzyPrefixLength());
            }
            if (allowRewrite && possibleFloat(*qt, ssTerm) && factory.getRewriteFloatTerms(ssIndex)) {
                auto phrase = std::make_unique<PhraseQueryNode>();
                auto dotPos = ssTerm.find('.');
                phrase->addChild(std::make_unique<QueryTerm>(factory.create(), ssTerm.substr(0, dotPos), ssIndex, TermType::WORD));
                phrase->addChild(std::make_unique<QueryTerm>(factory.create(), ssTerm.substr(dotPos + 1), ssIndex, TermType::WORD));
                auto orqn = std::make_unique<EquivQueryNode>();
                orqn->addChild(std::move(qt));
                orqn->addChild(std::move(phrase));
                qn = std::move(orqn);
            } else {
                qn = std::move(qt);
            }
        }
    }
    break;
    case ParseItem::ITEM_RANK:
    {
        if (arity >= 1) {
            queryRep.next();
            qn = Build(parent, factory, queryRep, false);
            for (uint32_t skipCount = arity-1; (skipCount > 0) && queryRep.next(); skipCount--) {
                skipCount += queryRep.getArity();
            }
        }
    }
    break;
    case ParseItem::ITEM_STRING_IN:
    case ParseItem::ITEM_NUMERIC_IN:
        qn = std::make_unique<InTerm>(factory.create(), queryRep.getIndexName(), queryRep.get_terms());
        break;
    case ParseItem::ITEM_DOT_PRODUCT:
        qn = build_dot_product_term(factory, queryRep);
        break;
    default:
        skip_unknown(queryRep);
        break;
    }
    return qn;
}

const HitList & QueryNode::evaluateHits(HitList & hl) const
{
    return hl;
}

std::unique_ptr<QueryNode>
QueryNode::build_nearest_neighbor_query_node(const QueryNodeResultFactory& factory, SimpleQueryStackDumpIterator& query_rep)
{
    vespalib::stringref query_tensor_name = query_rep.getTerm();
    vespalib::stringref field_name = query_rep.getIndexName();
    int32_t unique_id = query_rep.getUniqueId();
    auto weight = query_rep.GetWeight();
    uint32_t target_hits = query_rep.getTargetHits();
    double distance_threshold = query_rep.getDistanceThreshold();
    return std::make_unique<NearestNeighborQueryNode>(factory.create(),
                                                      query_tensor_name,
                                                      field_name,
                                                      target_hits,
                                                      distance_threshold,
                                                      unique_id,
                                                      weight);
}

void
QueryNode::populate_multi_term(MultiTerm& mt, SimpleQueryStackDumpIterator& queryRep)
{
    char buf[24];
    vespalib::string subterm;
    auto arity = queryRep.getArity();
    for (size_t i = 0; i < arity && queryRep.next(); i++) {
        std::unique_ptr<QueryTerm> term;
        switch (queryRep.getType()) {
        case ParseItem::ITEM_PURE_WEIGHTED_STRING:
            term = std::make_unique<QueryTerm>(std::unique_ptr<QueryNodeResultBase>(), queryRep.getTerm(), "", QueryTermSimple::Type::WORD);
            break;
        case ParseItem::ITEM_PURE_WEIGHTED_LONG:
        {
            auto res = std::to_chars(buf, buf + sizeof(buf), queryRep.getIntergerTerm(), 10);
            subterm.assign(buf, res.ptr - buf);
            term = std::make_unique<QueryTerm>(std::unique_ptr<QueryNodeResultBase>(), subterm, "", QueryTermSimple::Type::WORD);
        }
        break;
        default:
            skip_unknown(queryRep);
            break;
        }
        if (term) {
            term->setWeight(queryRep.GetWeight());
            mt.add_term(std::move(term));
        }
    }
}

std::unique_ptr<QueryNode>
QueryNode::build_dot_product_term(const QueryNodeResultFactory& factory, SimpleQueryStackDumpIterator& queryRep)
{
    auto dp =std::make_unique<DotProductTerm>(factory.create(), queryRep.getIndexName(), queryRep.getArity());
    dp->setWeight(queryRep.GetWeight());
    dp->setUniqueId(queryRep.getUniqueId());
    populate_multi_term(*dp, queryRep);
    return dp;
}

void
QueryNode::skip_unknown(SimpleQueryStackDumpIterator& queryRep)
{
    auto type = queryRep.getType();
    for (uint32_t skipCount = queryRep.getArity(); (skipCount > 0) && queryRep.next(); skipCount--) {
        skipCount += queryRep.getArity();
        LOG(warning, "Does not understand anything,.... skipping %d", type);
    }
}

}
