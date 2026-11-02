#include <vector>
#include <iostream>
#include <fstream>
#include <string>
#include <cstdlib>

static constexpr int Utf8TwoByteSequenceLength = 2;
static constexpr int Utf8ThreeByteSequenceLength = 3;
static constexpr int Utf8FourByteSequenceLength = 4;
static constexpr int ExpectedArgumentCount = 3;
static constexpr int TargetArgumentIndex = 1;
static constexpr int FilenameArgumentIndex = 2;
static constexpr int ErrorExitCode = -2;

/*
 * The utf8_check() function scans the '\0'-terminated string starting
 * at s. It returns a pointer to the first byte of the first malformed
 * or overlong UTF-8 sequence found, or NULL if the string contains
 * only correct UTF-8. It also spots UTF-8 sequences that could cause
 * trouble if converted to UTF-16, namely surrogate characters
 * (U+D800..U+DFFF) and non-Unicode positions (U+FFFE..U+FFFF). This
 * routine is very likely to find a malformed sequence if the input
 * uses any other encoding than UTF-8. It therefore can be used as a
 * very effective heuristic for distinguishing between UTF-8 and other
 * encodings.
 *
 * I wrote this code mainly as a specification of functionality; there
 * are no doubt performance optimizations possible for certain CPUs.
 *
 * Markus Kuhn <http://www.cl.cam.ac.uk/~mgk25/> -- 2005-03-30
 * License: http://www.cl.cam.ac.uk/~mgk25/short-license.html
 */

unsigned char *utf8_check(unsigned char *s)
{
    while (*s) {
        if (*s < 0x80) {
            // 0xxxxxxx
            s++;
        } else if ((s[0] & 0xe0) == 0xc0) {
            // 110xxxxx 10xxxxxx
            if ((s[1] & 0xc0) != 0x80 ||
                (s[0] & 0xfe) == 0xc0) {         // overlong?
                return s;
            } else {
                s += Utf8TwoByteSequenceLength;
            }
        } else if ((s[0] & 0xf0) == 0xe0) {
            // 1110xxxx 10xxxxxx 10xxxxxx
            if ((s[1] & 0xc0) != 0x80 ||
                (s[Utf8TwoByteSequenceLength] & 0xc0) != 0x80 ||
                (s[0] == 0xe0 && (s[1] & 0xe0) == 0x80) || // overlong?
                (s[0] == 0xed && (s[1] & 0xe0) == 0xa0) || // surrogate?
                (s[0] == 0xef && s[1] == 0xbf &&
                (s[Utf8TwoByteSequenceLength] & 0xfe) == 0xbe)) {                  // U+FFFE or U+FFFF?
                return s;
            } else {
                s += Utf8ThreeByteSequenceLength;
            }
        } else if ((s[0] & 0xf8) == 0xf0) {
            // 11110xxX 10xxxxxx 10xxxxxx 10xxxxxx
            if ((s[1] & 0xc0) != 0x80 ||
                (s[Utf8TwoByteSequenceLength] & 0xc0) != 0x80 ||
                (s[Utf8ThreeByteSequenceLength] & 0xc0) != 0x80 ||
                (s[0] == 0xf0 && (s[1] & 0xf0) == 0x80) ||      // overlong?
                (s[0] == 0xf4 && s[1] > 0x8f) || s[0] > 0xf4) { // > U+10FFFF?
                return s;
            } else {
                s += Utf8FourByteSequenceLength;
            }
        } else {
            return s;
        }
    }

    return NULL;
}


int main(int argc, char const *argv[])
{
    if (argc != ExpectedArgumentCount) {
        std::cerr << "Usage: " << argv[0] << " <program/library> <file>" << std::endl;
        return -1;
    }

    const char* target = argv[TargetArgumentIndex];
    const char* filename = argv[FilenameArgumentIndex];

    const auto error_exit = [=](const char* error) {
        std::cerr << "Target: " << target << " @"<< filename << "\n\tError: " << error << std::endl;
        std::exit(ErrorExitCode);
    };

    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    const std::streampos size = file.tellg();

    if (size == std::streampos{ 0 }) {
        return 0;
    }

    file.seekg(0, std::ios::beg);
    std::vector<char> buffer(size);

    if (file.read(buffer.data(), size)) {
        buffer.push_back('\0');

        // Check UTF-8 validity
        unsigned char* start = reinterpret_cast<unsigned char*>(buffer.data());
        unsigned char* problem = utf8_check(start);
        if (problem != nullptr) {
            std::string msg = "Source file does not contain (valid) UTF-8 @pos:";
            msg += std::to_string(size_t(problem) - size_t(start));
            msg += " with these characters before: '";
            for(unsigned char* c=std::max(start, problem-10); c<problem; ++c)
                msg+= *c;
            msg += "'";
            error_exit(msg.c_str());
        }

        // Check against a BOM mark
        if (buffer.size() >= 3
            && buffer[0] == '\xef'
            && buffer[1] == '\xbb'
            && buffer[2] == '\xbf') {
            error_exit("Source file is valid UTF-8 but contains a BOM mark");
        }
    } else {
        error_exit("Could not read source file");
    }

    return 0;
}
