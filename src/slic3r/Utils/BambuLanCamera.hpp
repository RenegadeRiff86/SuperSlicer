///|/ Copyright (c) 2026 Stan Elston
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_BambuLanCamera_hpp_
#define slic3r_BambuLanCamera_hpp_

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {

// Developer LAN camera. The X1 Carbon serves an encrypted stream on port 322.
// Decoding needs libav and is omitted when those libraries were not found, so
// the rest of the printer controls still build. The access code is used only
// inside this process.
class BambuLanCamera
{
public:
    struct Frame
    {
        int width = 0;
        int height = 0;
        std::vector<unsigned char> rgb;
    };

    using FrameFn = std::function<void(Frame frame)>;
    using ErrorFn = std::function<void(const std::string &error)>;

    BambuLanCamera();
    ~BambuLanCamera();

    BambuLanCamera(const BambuLanCamera &) = delete;
    BambuLanCamera &operator=(const BambuLanCamera &) = delete;

    static bool available();

    // on_frame and on_error run on the decode thread.
    void start(const std::string &host, const std::string &access_code, FrameFn on_frame, ErrorFn on_error);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}

#endif
