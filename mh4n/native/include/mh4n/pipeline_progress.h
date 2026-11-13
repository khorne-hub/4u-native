// How far the start's pipeline list is compiled (gpu/pipeline/caching_provider.cpp), for the app's plate "Building
// shaders" (1.0's S30): the list is the pipelines the earlier sessions drew, compiled on the workers from the start so a
// draw finds its own pipeline ready; a session whose list is long or whose driver cache is cold (a first start, a cleared
// cache, a new driver) compiles for seconds or minutes, and its draws without a pipeline yet are left out meanwhile.
#pragma once

#include <cstdint>

namespace mh4n::pipeline_progress {

struct State {
    uint32_t done = 0;       // keys of the list built (or refused by the driver) so far
    uint32_t total = 0;      // keys in the list
    bool compiling = false;  // the list is still being compiled
};

// Any thread.
State Get();

}  // namespace mh4n::pipeline_progress
