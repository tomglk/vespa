# Copyright Vespa.ai. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
vespa_add_library(searchlib_query_streaming OBJECT
    SOURCES
    dot_product_term.cpp
    in_term.cpp
    multi_term.cpp
    nearest_neighbor_query_node.cpp
    query.cpp
    querynode.cpp
    querynoderesultbase.cpp
    queryterm.cpp
    DEPENDS
)
