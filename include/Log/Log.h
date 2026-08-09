#pragma once
#include <Index.h>
#include <Segment.h>
#include <Config.h>
#include <vector>

class Log {
    public:
    private:
        std::string dir;
        Segment *active_segment;
        std::vector<Segment *> segments;
}