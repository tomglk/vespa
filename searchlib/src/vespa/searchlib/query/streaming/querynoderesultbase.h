// Copyright Vespa.ai. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
#pragma once

#include <vespa/vespalib/stllike/string.h>
#include <memory>

namespace search::streaming {

/**
  This is the base of any item that can be attached to the leafs in a querytree.
  The intention is to put stuff here that are search specific. Fx to differentiate
  between streamed and indexed variants.
*/
class QueryNodeResultBase
{
public:
    virtual ~QueryNodeResultBase() = default;
    virtual QueryNodeResultBase * clone() const = 0;
};

class QueryNodeResultFactory {
public:
    virtual ~QueryNodeResultFactory() = default;
    virtual bool getRewriteFloatTerms(vespalib::stringref index) const noexcept { (void) index; return false; }
    virtual std::unique_ptr<QueryNodeResultBase> create() const { return {}; }
};
}

