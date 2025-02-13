// Copyright Vespa.ai. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#pragma once

#include "multi_term.h"

namespace search::streaming {

/*
 * A dot product query term for streaming search.
 */
class DotProductTerm : public MultiTerm {
public:
    DotProductTerm(std::unique_ptr<QueryNodeResultBase> result_base, const string& index, uint32_t num_terms);
    ~DotProductTerm() override;
    void unpack_match_data(uint32_t docid, const fef::ITermData& td, fef::MatchData& match_data) override;
};

}
