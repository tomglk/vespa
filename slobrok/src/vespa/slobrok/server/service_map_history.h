// Copyright Verizon Media. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#pragma once

#include <vespa/vespalib/stllike/string.h>
#include <vespa/vespalib/util/gencnt.h>
#include <vespa/vespalib/util/arrayqueue.hpp>
#include <map>
#include <mutex>
#include "service_mapping.h"
#include "map_diff.h"

namespace slobrok {

/**
 * @class ServiceMapHistory
 * @brief API to generate incremental updates for a collection of name->spec mappings
 **/

class ServiceMapHistory
{
public:
    using Generation = vespalib::GenCnt;

    /** implement this interface to receive the result of an async generation diff */
    class DiffCompletionHandler
    {
    public:
        /**
         * Handle the result of asyncGenerationDiff()
         *
         * @param updateDiff changes from the generation requested
         **/
        virtual void handle(MapDiff updateDiff) = 0;
    protected:
        virtual ~DiffCompletionHandler() {}
    };

private:
    struct UpdateLog {
        static constexpr uint32_t keep_items = 1000;
        Generation startGeneration;
        Generation currentGeneration;
        vespalib::ArrayQueue<vespalib::string> updates;
        UpdateLog();
        ~UpdateLog();
        void add(const vespalib::string &name);
        bool isInRange(const Generation &gen) const;
        std::vector<vespalib::string> updatedSince(const Generation &gen) const;
    };
    using Map = std::map<vespalib::string, vespalib::string>;
    using Waiter = std::pair<DiffCompletionHandler *, Generation>;
    using WaitList = std::vector<Waiter>;

    mutable std::mutex _lock;
    Map        _map;
    WaitList   _waitList;
    UpdateLog  _log;

    void notify_updated();
 
    const Generation &myGen() const { return _log.currentGeneration; }

public:
    ServiceMapHistory();
    ~ServiceMapHistory();

    /**
     * Ask for notification when the history has changes newer than fromGen.
     * Note that if there are any changes in the history already, the callback
     * will happen immediately (inside asyncGenerationDiff).
     **/
    void asyncGenerationDiff(DiffCompletionHandler *handler, const Generation &fromGen);

    /**
     * Cancel pending notification.
     * @return true if handler was canceled without calling handle() at all.
     **/
    bool cancel(DiffCompletionHandler *handler);

    /** add or update name->spec mapping */
    void update(const ServiceMapping &mapping);

    /** remove mapping for name */
    void remove(const vespalib::string &name);

    /** For unit testing only: */
    Generation currentGen() const { return myGen(); }

private:
    MapDiff makeDiffFrom(const Generation &fromGen) const;
};

//-----------------------------------------------------------------------------

} // namespace slobrok

