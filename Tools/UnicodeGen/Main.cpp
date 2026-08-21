// The Unicode table generator.
//
// Reads the Unicode Character Database files under Data/ and writes the generated tables of Packages/Unicode as Rux
// source. Run by hand when the data changes; the output is committed, so an ordinary build never runs this and the
// package needs no network and no generator to compile.
//
// Every generated file records the UCD version and the SHA-256 of each input it was generated from, which is what
// makes a regeneration checkable: same hashes in, same tables out, or the diff explains itself.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ---- SHA-256, for the source hashes the headers record. ----

struct Sha256 {
    std::array<std::uint32_t, 8> state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<std::uint8_t> buffer;
    std::uint64_t total = 0;

    static std::uint32_t Rot(std::uint32_t value, int by) { return (value >> by) | (value << (32 - by)); }

    void Block(const std::uint8_t *p) {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t(p[i * 4]) << 24) | (std::uint32_t(p[i * 4 + 1]) << 16) |
                   (std::uint32_t(p[i * 4 + 2]) << 8) | std::uint32_t(p[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = Rot(w[i - 15], 7) ^ Rot(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = Rot(w[i - 2], 17) ^ Rot(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto s = state;
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 = Rot(s[4], 6) ^ Rot(s[4], 11) ^ Rot(s[4], 25);
            const std::uint32_t ch = (s[4] & s[5]) ^ (~s[4] & s[6]);
            const std::uint32_t t1 = s[7] + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 = Rot(s[0], 2) ^ Rot(s[0], 13) ^ Rot(s[0], 22);
            const std::uint32_t mj = (s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]);
            const std::uint32_t t2 = s0 + mj;
            s[7] = s[6];
            s[6] = s[5];
            s[5] = s[4];
            s[4] = s[3] + t1;
            s[3] = s[2];
            s[2] = s[1];
            s[1] = s[0];
            s[0] = t1 + t2;
        }
        for (int i = 0; i < 8; ++i) {
            state[i] += s[i];
        }
    }

    void Update(const std::uint8_t *data, std::size_t length) {
        total += length;
        buffer.insert(buffer.end(), data, data + length);
        std::size_t offset = 0;
        while (buffer.size() - offset >= 64) {
            Block(buffer.data() + offset);
            offset += 64;
        }
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    std::string Finish() {
        const std::uint64_t bits = total * 8;
        std::uint8_t pad[72] = {0x80};
        const std::size_t padLength = (buffer.size() % 64 < 56) ? 56 - buffer.size() % 64 : 120 - buffer.size() % 64;
        Update(pad, padLength);
        std::uint8_t lengthBytes[8];
        for (int i = 0; i < 8; ++i) {
            lengthBytes[i] = std::uint8_t(bits >> (56 - i * 8));
        }
        Update(lengthBytes, 8);
        std::string out;
        for (const std::uint32_t word : state) {
            out += std::format("{:08x}", word);
        }
        return out;
    }
};

std::string FileSha256(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    Sha256 hash;
    char chunk[65536];
    while (in.read(chunk, sizeof chunk) || in.gcount() > 0) {
        hash.Update(reinterpret_cast<const std::uint8_t *>(chunk), static_cast<std::size_t>(in.gcount()));
    }
    return hash.Finish();
}

// ---- UCD parsing. ----

std::vector<std::string> SplitFields(const std::string &line, const char separator) {
    std::vector<std::string> fields;
    std::string field;
    for (const char ch : line) {
        if (ch == separator) {
            fields.push_back(field);
            field.clear();
        } else {
            field += ch;
        }
    }
    fields.push_back(field);
    return fields;
}

std::string Trim(const std::string &text) {
    const std::size_t begin = text.find_first_not_of(" \t\r");
    const std::size_t end = text.find_last_not_of(" \t\r");
    return begin == std::string::npos ? std::string() : text.substr(begin, end - begin + 1);
}

std::uint32_t Hex(const std::string &text) {
    return static_cast<std::uint32_t>(std::stoul(text, nullptr, 16));
}

std::vector<std::uint32_t> HexSequence(const std::string &text) {
    std::vector<std::uint32_t> out;
    std::stringstream stream(text);
    std::string piece;
    while (stream >> piece) {
        out.push_back(Hex(piece));
    }
    return out;
}

// One data line of a property file: a code point or range, then the property it carries.
struct PropertyLine {
    std::uint32_t first = 0;
    std::uint32_t last = 0;
    std::string property;
    // A property with a value, `InCB; Linker` among them, carries it in a third field.
    std::string value;
};

std::vector<PropertyLine> ReadPropertyFile(const std::filesystem::path &path) {
    std::vector<PropertyLine> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line = line.substr(0, comment);
        }
        if (Trim(line).empty()) {
            continue;
        }
        const auto fields = SplitFields(line, ';');
        if (fields.size() < 2) {
            continue;
        }
        PropertyLine entry;
        const std::string range = Trim(fields[0]);
        const std::size_t dots = range.find("..");
        if (dots == std::string::npos) {
            entry.first = entry.last = Hex(range);
        } else {
            entry.first = Hex(range.substr(0, dots));
            entry.last = Hex(range.substr(dots + 2));
        }
        entry.property = Trim(fields[1]);
        if (fields.size() >= 3) {
            entry.value = Trim(fields[2]);
        }
        out.push_back(entry);
    }
    return out;
}

// ---- Range compression: adjacent code points sharing a value fold into one (first, last, value) triple. ----

struct ValueRange {
    std::uint32_t first;
    std::uint32_t last;
    std::uint32_t value;
};

std::vector<ValueRange> Compress(const std::map<std::uint32_t, std::uint32_t> &values) {
    std::vector<ValueRange> out;
    for (const auto &[cp, value] : values) {
        if (!out.empty() && out.back().last + 1 == cp && out.back().value == value) {
            out.back().last = cp;
        } else {
            out.push_back({cp, cp, value});
        }
    }
    return out;
}

std::vector<ValueRange> CompressSet(const std::set<std::uint32_t> &members) {
    std::map<std::uint32_t, std::uint32_t> values;
    for (const std::uint32_t cp : members) {
        values[cp] = 1;
    }
    return Compress(values);
}

// ---- Rux emission. ----

// A generated file: a header naming the sources and their hashes, then flat uint32 arrays. Twelve values per line
// keeps every line within the column limit whatever the values are.
struct RuxWriter {
    std::string text;

    void Header(const std::string &what, const std::vector<std::pair<std::string, std::string>> &hashes) {
        text += "// " + what + "\n//\n";
        text += "// GENERATED by Tools/UnicodeGen from the Unicode 17.0.0 Character Database. Do not edit by hand:\n";
        text += "// regenerate with `Bin/Tools/rux-unicode-gen` after changing the data, and expect a stable output\n";
        text += "// for unchanged inputs. The inputs were:\n//\n";
        for (const auto &[name, hash] : hashes) {
            text += "//   " + name + "\n//     sha256 " + hash + "\n";
        }
        text += "\n";
    }

    void Array(const std::string &doc, const std::string &name, const std::vector<std::uint32_t> &values) {
        // The doc line wraps at the column limit, on word boundaries, so lint never has to.
        std::string rest = doc;
        while (!rest.empty()) {
            if (rest.size() <= 112) {
                text += "/// " + rest + "\n";
                break;
            }
            std::size_t cut = rest.rfind(' ', 112);
            if (cut == std::string::npos) {
                cut = 112;
            }
            text += "/// " + rest.substr(0, cut) + "\n";
            rest = rest.substr(cut + 1);
        }
        text += "///\n/// https://rux-lang.dev/docs/api/unicode/tables\n";
        text += std::format("const {}: uint32[{}] = [\n", name, values.size());
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (index % 12 == 0) {
                text += "    ";
            }
            text += std::to_string(values[index]);
            if (index + 1 != values.size()) {
                text += ",";
            }
            if (index % 12 == 11 || index + 1 == values.size()) {
                text += "\n";
            } else {
                text += " ";
            }
        }
        text += "];\n\n";
    }

    void Write(const std::filesystem::path &path) {
        std::ofstream out(path, std::ios::binary);
        out << text;
        std::printf("wrote %s (%zu bytes)\n", path.string().c_str(), text.size());
    }
};

std::vector<std::uint32_t> FlattenRanges(const std::vector<ValueRange> &ranges, const bool withValue) {
    std::vector<std::uint32_t> out;
    for (const auto &range : ranges) {
        out.push_back(range.first);
        out.push_back(range.last);
        if (withValue) {
            out.push_back(range.value);
        }
    }
    return out;
}

// ---- The database, read once and consulted by every emitter. ----

struct CodePoint {
    std::string category;
    std::uint32_t combiningClass = 0;
    bool compatibilityDecomposition = false;
    std::vector<std::uint32_t> decomposition;
    std::optional<std::uint32_t> simpleUpper;
    std::optional<std::uint32_t> simpleLower;
    std::optional<std::uint32_t> simpleTitle;
};

struct Database {
    std::map<std::uint32_t, CodePoint> points;
    std::vector<PropertyLine> propList;
    std::vector<PropertyLine> coreProperties;
    std::vector<PropertyLine> graphemeBreaks;
    std::vector<PropertyLine> emoji;
    std::set<std::uint32_t> compositionExclusions;
    std::map<std::uint32_t, std::vector<std::uint32_t>> fullFold;
    std::map<std::uint32_t, std::vector<std::uint32_t>> fullUpper;
    std::map<std::uint32_t, std::vector<std::uint32_t>> fullLower;
};

void ReadUnicodeData(Database &db, const std::filesystem::path &path) {
    std::ifstream in(path);
    std::string line;
    std::optional<std::pair<std::uint32_t, CodePoint>> rangeFirst;
    while (std::getline(in, line)) {
        const auto fields = SplitFields(line, ';');
        if (fields.size() < 15) {
            continue;
        }
        const std::uint32_t cp = Hex(fields[0]);
        CodePoint point;
        point.category = fields[2];
        point.combiningClass = static_cast<std::uint32_t>(std::stoul(fields[3]));
        if (!fields[5].empty()) {
            std::string decomposition = fields[5];
            if (decomposition[0] == '<') {
                point.compatibilityDecomposition = true;
                decomposition = Trim(decomposition.substr(decomposition.find('>') + 1));
            }
            point.decomposition = HexSequence(decomposition);
        }
        if (!fields[12].empty()) {
            point.simpleUpper = Hex(fields[12]);
        }
        if (!fields[13].empty()) {
            point.simpleLower = Hex(fields[13]);
        }
        if (!fields[14].empty()) {
            point.simpleTitle = Hex(fields[14]);
        }

        // A range is two lines, "<Name, First>" and "<Name, Last>", and every code point between them shares the
        // first line's fields.
        if (fields[1].find(", First>") != std::string::npos) {
            rangeFirst = {cp, point};
            continue;
        }
        if (fields[1].find(", Last>") != std::string::npos && rangeFirst) {
            for (std::uint32_t each = rangeFirst->first; each <= cp; ++each) {
                db.points[each] = rangeFirst->second;
            }
            rangeFirst.reset();
            continue;
        }
        db.points[cp] = point;
    }
}

void ReadCompositionExclusions(Database &db, const std::filesystem::path &path) {
    // Unlike every property file, this one carries bare code points with no semicolon after them, so the shared
    // reader would see one field and skip every line.
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line = line.substr(0, comment);
        }
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        const std::size_t dots = line.find("..");
        const std::uint32_t first = dots == std::string::npos ? Hex(line) : Hex(line.substr(0, dots));
        const std::uint32_t last = dots == std::string::npos ? first : Hex(line.substr(dots + 2));
        for (std::uint32_t cp = first; cp <= last; ++cp) {
            db.compositionExclusions.insert(cp);
        }
    }
}

void ReadCaseFolding(Database &db, const std::filesystem::path &path) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line = line.substr(0, comment);
        }
        if (Trim(line).empty()) {
            continue;
        }
        const auto fields = SplitFields(line, ';');
        if (fields.size() < 3) {
            continue;
        }
        // C and F together are the full, locale-independent folding; S is the simple alternative to F and T the
        // Turkic special case, and taking either would change answers by locale, which folding exists to avoid.
        const std::string status = Trim(fields[1]);
        if (status != "C" && status != "F") {
            continue;
        }
        db.fullFold[Hex(Trim(fields[0]))] = HexSequence(Trim(fields[2]));
    }
}

void ReadSpecialCasing(Database &db, const std::filesystem::path &path) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line = line.substr(0, comment);
        }
        if (Trim(line).empty()) {
            continue;
        }
        const auto fields = SplitFields(line, ';');
        if (fields.size() < 4) {
            continue;
        }
        // A fifth non-empty field is a condition -- a locale, or a context like Final_Sigma -- and conditional
        // mappings are exactly what locale-independent case conversion must not apply.
        if (fields.size() >= 5 && !Trim(fields[4]).empty()) {
            continue;
        }
        const std::uint32_t cp = Hex(Trim(fields[0]));
        db.fullLower[cp] = HexSequence(Trim(fields[1]));
        db.fullUpper[cp] = HexSequence(Trim(fields[3]));
    }
}

// ---- The emitters, one generated file per concern. ----

// The general categories, numbered in a fixed order the Rux enum mirrors. Order is meaning here: the table stores
// the ordinal, so the enum in `Category.rux` must list them identically.
const std::vector<std::string> &CategoryNames() {
    static const std::vector<std::string> names = {
        "Cn", "Lu", "Ll", "Lt", "Lm", "Lo", "Mn", "Mc", "Me", "Nd", "Nl", "No", "Pc", "Pd", "Ps",
        "Pe", "Pi", "Pf", "Po", "Sm", "Sc", "Sk", "So", "Zs", "Zl", "Zp", "Cc", "Cf", "Cs", "Co"};
    return names;
}

std::uint32_t CategoryOrdinal(const std::string &name) {
    const auto &names = CategoryNames();
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (names[index] == name) {
            return static_cast<std::uint32_t>(index);
        }
    }
    std::fprintf(stderr, "unknown category %s\n", name.c_str());
    std::exit(1);
}

void EmitProperties(const Database &db, const std::filesystem::path &out,
                    const std::vector<std::pair<std::string, std::string>> &hashes) {
    std::map<std::uint32_t, std::uint32_t> categories;
    std::map<std::uint32_t, std::uint32_t> combining;
    for (const auto &[cp, point] : db.points) {
        const std::uint32_t ordinal = CategoryOrdinal(point.category);
        if (ordinal != 0) {
            categories[cp] = ordinal;
        }
        if (point.combiningClass != 0) {
            combining[cp] = point.combiningClass;
        }
    }

    std::set<std::uint32_t> whiteSpace;
    for (const auto &entry : db.propList) {
        if (entry.property == "White_Space") {
            for (std::uint32_t cp = entry.first; cp <= entry.last; ++cp) {
                whiteSpace.insert(cp);
            }
        }
    }
    std::set<std::uint32_t> alphabetic;
    for (const auto &entry : db.coreProperties) {
        if (entry.property == "Alphabetic") {
            for (std::uint32_t cp = entry.first; cp <= entry.last; ++cp) {
                alphabetic.insert(cp);
            }
        }
    }
    std::set<std::uint32_t> numeric;
    for (const auto &[cp, point] : db.points) {
        if (point.category == "Nd" || point.category == "Nl" || point.category == "No") {
            numeric.insert(cp);
        }
    }

    RuxWriter writer;
    writer.Header("The character property tables: general category, combining class, and the boolean properties.",
                  hashes);
    writer.Array("General category ranges: first, last, category ordinal. Sorted by first; a code point in no "
                 "range is unassigned.",
                 "CategoryRanges", FlattenRanges(Compress(categories), true));
    writer.Array("Canonical combining class ranges: first, last, class. A code point in no range has class zero, "
                 "which is nearly all of them.",
                 "CombiningClassRanges", FlattenRanges(Compress(combining), true));
    writer.Array("White_Space ranges: first, last.", "WhiteSpaceRanges",
                 FlattenRanges(CompressSet(whiteSpace), false));
    writer.Array("Alphabetic ranges: first, last.", "AlphabeticRanges",
                 FlattenRanges(CompressSet(alphabetic), false));
    writer.Array("Numeric ranges -- the Nd, Nl and No categories: first, last.", "NumericRanges",
                 FlattenRanges(CompressSet(numeric), false));
    writer.Write(out);
}

void EmitCase(const Database &db, const std::filesystem::path &out,
              const std::vector<std::pair<std::string, std::string>> &hashes) {
    std::map<std::uint32_t, std::uint32_t> upper;
    std::map<std::uint32_t, std::uint32_t> lower;
    std::map<std::uint32_t, std::uint32_t> title;
    for (const auto &[cp, point] : db.points) {
        if (point.simpleUpper && *point.simpleUpper != cp) {
            upper[cp] = *point.simpleUpper;
        }
        if (point.simpleLower && *point.simpleLower != cp) {
            lower[cp] = *point.simpleLower;
        }
        if (point.simpleTitle && *point.simpleTitle != cp) {
            title[cp] = *point.simpleTitle;
        }
    }

    const auto pairs = [](const std::map<std::uint32_t, std::uint32_t> &mapping) {
        std::vector<std::uint32_t> flat;
        for (const auto &[cp, mapped] : mapping) {
            flat.push_back(cp);
            flat.push_back(mapped);
        }
        return flat;
    };
    const auto pool = [](const std::map<std::uint32_t, std::vector<std::uint32_t>> &mapping,
                         std::vector<std::uint32_t> &index) {
        std::vector<std::uint32_t> elements;
        for (const auto &[cp, sequence] : mapping) {
            index.push_back(cp);
            index.push_back(static_cast<std::uint32_t>(elements.size()));
            index.push_back(static_cast<std::uint32_t>(sequence.size()));
            elements.insert(elements.end(), sequence.begin(), sequence.end());
        }
        return elements;
    };

    RuxWriter writer;
    writer.Header("The case tables: simple mappings, the unconditional full mappings, and full case folding.",
                  hashes);
    writer.Array("Simple uppercase pairs: code point, its uppercase. Sorted; absence means the point maps to "
                 "itself.",
                 "SimpleUpperPairs", pairs(upper));
    writer.Array("Simple lowercase pairs: code point, its lowercase.", "SimpleLowerPairs", pairs(lower));
    writer.Array("Simple titlecase pairs, for the points whose titlecase differs from their uppercase.",
                 "SimpleTitlePairs", pairs(title));

    std::vector<std::uint32_t> foldIndex;
    const auto foldPool = pool(db.fullFold, foldIndex);
    writer.Array("Full case folding index: code point, offset, length. C and F entries only, so the folding is "
                 "the same in every locale.",
                 "FullFoldIndex", foldIndex);
    writer.Array("Full case folding elements, indexed by FullFoldIndex.", "FullFoldElements", foldPool);

    std::vector<std::uint32_t> upperIndex;
    const auto upperPool = pool(db.fullUpper, upperIndex);
    writer.Array("Unconditional full uppercase index: code point, offset, length. The conditional mappings are "
                 "deliberately absent -- they depend on locale or context.",
                 "FullUpperIndex", upperIndex);
    writer.Array("Unconditional full uppercase elements.", "FullUpperElements", upperPool);

    std::vector<std::uint32_t> lowerIndex;
    const auto lowerPool = pool(db.fullLower, lowerIndex);
    writer.Array("Unconditional full lowercase index: code point, offset, length.", "FullLowerIndex", lowerIndex);
    writer.Array("Unconditional full lowercase elements.", "FullLowerElements", lowerPool);
    writer.Write(out);
}

// The full canonical (or compatibility) decomposition of one code point, expanded recursively so the runtime never
// recurses. Hangul is deliberately absent: its decomposition is arithmetic, and the runtime computes it.
std::vector<std::uint32_t> FullDecomposition(const Database &db, const std::uint32_t cp, const bool compatibility) {
    const auto found = db.points.find(cp);
    if (found == db.points.end() || found->second.decomposition.empty() ||
        (!compatibility && found->second.compatibilityDecomposition)) {
        return {cp};
    }
    std::vector<std::uint32_t> out;
    for (const std::uint32_t piece : found->second.decomposition) {
        const auto expanded = FullDecomposition(db, piece, compatibility);
        out.insert(out.end(), expanded.begin(), expanded.end());
    }
    return out;
}

void EmitNormalization(const Database &db, const std::filesystem::path &out,
                       const std::vector<std::pair<std::string, std::string>> &hashes) {
    std::map<std::uint32_t, std::vector<std::uint32_t>> canonical;
    std::map<std::uint32_t, std::vector<std::uint32_t>> compatibility;
    for (const auto &[cp, point] : db.points) {
        if (point.decomposition.empty()) {
            continue;
        }
        const auto full = FullDecomposition(db, cp, false);
        if (!(full.size() == 1 && full[0] == cp)) {
            canonical[cp] = full;
        }
        const auto compat = FullDecomposition(db, cp, true);
        if (!(compat.size() == 1 && compat[0] == cp)) {
            compatibility[cp] = compat;
        }
    }

    // The primary composites: every canonical pair decomposition whose composed form is not excluded and whose
    // decomposition does not start with a combining mark. Composition is the inverse of exactly these.
    std::vector<std::array<std::uint32_t, 3>> composition;
    for (const auto &[cp, point] : db.points) {
        if (point.compatibilityDecomposition || point.decomposition.size() != 2) {
            continue;
        }
        if (db.compositionExclusions.contains(cp)) {
            continue;
        }
        const auto starter = db.points.find(point.decomposition[0]);
        if (starter != db.points.end() && starter->second.combiningClass != 0) {
            continue;
        }
        composition.push_back({point.decomposition[0], point.decomposition[1], cp});
    }
    std::ranges::sort(composition);

    const auto pool = [](const std::map<std::uint32_t, std::vector<std::uint32_t>> &mapping,
                         std::vector<std::uint32_t> &index) {
        std::vector<std::uint32_t> elements;
        for (const auto &[cp, sequence] : mapping) {
            index.push_back(cp);
            index.push_back(static_cast<std::uint32_t>(elements.size()));
            index.push_back(static_cast<std::uint32_t>(sequence.size()));
            elements.insert(elements.end(), sequence.begin(), sequence.end());
        }
        return elements;
    };

    RuxWriter writer;
    writer.Header("The normalization tables: full decompositions, and the primary composites.", hashes);
    std::vector<std::uint32_t> canonicalIndex;
    const auto canonicalPool = pool(canonical, canonicalIndex);
    writer.Array("Canonical decomposition index: code point, offset, length. Fully expanded, so lookup never "
                 "recurses. Hangul is absent: its decomposition is arithmetic and computed instead.",
                 "CanonicalDecompositionIndex", canonicalIndex);
    writer.Array("Canonical decomposition elements.", "CanonicalDecompositionElements", canonicalPool);
    std::vector<std::uint32_t> compatibilityIndex;
    const auto compatibilityPool = pool(compatibility, compatibilityIndex);
    writer.Array("Compatibility decomposition index: code point, offset, length.", "CompatibilityDecompositionIndex",
                 compatibilityIndex);
    writer.Array("Compatibility decomposition elements.", "CompatibilityDecompositionElements", compatibilityPool);

    std::vector<std::uint32_t> flat;
    for (const auto &entry : composition) {
        flat.push_back(entry[0]);
        flat.push_back(entry[1]);
        flat.push_back(entry[2]);
    }
    writer.Array("Primary composite triples: starter, combining, composed. Sorted by starter then combining, for "
                 "binary search. The exclusions and non-starter decompositions are already left out.",
                 "CompositionTriples", flat);
    writer.Write(out);
}

void EmitGraphemes(const Database &db, const std::filesystem::path &out,
                   const std::vector<std::pair<std::string, std::string>> &hashes) {
    // The break classes, numbered in the order the Rux enum mirrors. `Any` is zero: a code point in no range.
    const std::vector<std::string> classes = {"Any",  "CR",          "LF",     "Control", "Extend", "ZWJ",
                                              "Regional_Indicator", "Prepend", "SpacingMark", "L",   "V",
                                              "T",    "LV",          "LVT"};
    std::map<std::uint32_t, std::uint32_t> values;
    for (const auto &entry : db.graphemeBreaks) {
        std::uint32_t ordinal = 0;
        for (std::size_t index = 0; index < classes.size(); ++index) {
            if (classes[index] == entry.property) {
                ordinal = static_cast<std::uint32_t>(index);
            }
        }
        if (ordinal == 0) {
            continue;
        }
        for (std::uint32_t cp = entry.first; cp <= entry.last; ++cp) {
            values[cp] = ordinal;
        }
    }
    std::set<std::uint32_t> pictographic;
    for (const auto &entry : db.emoji) {
        if (entry.property == "Extended_Pictographic") {
            for (std::uint32_t cp = entry.first; cp <= entry.last; ++cp) {
                pictographic.insert(cp);
            }
        }
    }

    // The InCB property drives the Indic conjunct rule, GB9c: a consonant, a linker somewhere in the marks, then
    // another consonant, all one cluster.
    std::set<std::uint32_t> conjunctConsonants;
    std::set<std::uint32_t> conjunctLinkers;
    std::set<std::uint32_t> conjunctExtends;
    for (const auto &entry : db.coreProperties) {
        if (entry.property != "InCB") {
            continue;
        }
        for (std::uint32_t cp = entry.first; cp <= entry.last; ++cp) {
            if (entry.value == "Consonant") {
                conjunctConsonants.insert(cp);
            } else if (entry.value == "Linker") {
                conjunctLinkers.insert(cp);
            } else if (entry.value == "Extend") {
                conjunctExtends.insert(cp);
            }
        }
    }

    RuxWriter writer;
    writer.Header("The grapheme cluster tables: break classes and the extended pictographs.", hashes);
    writer.Array("Grapheme break class ranges: first, last, class ordinal. The ordinals mirror `GraphemeBreak` in "
                 "Graphemes.rux, and a code point in no range is `Any`.",
                 "GraphemeBreakRanges", FlattenRanges(Compress(values), true));
    writer.Array("Extended_Pictographic ranges: first, last.", "PictographicRanges",
                 FlattenRanges(CompressSet(pictographic), false));
    writer.Array("InCB=Consonant ranges: first, last.", "ConjunctConsonantRanges",
                 FlattenRanges(CompressSet(conjunctConsonants), false));
    writer.Array("InCB=Linker ranges: first, last.", "ConjunctLinkerRanges",
                 FlattenRanges(CompressSet(conjunctLinkers), false));
    writer.Array("InCB=Extend ranges: first, last.", "ConjunctExtendRanges",
                 FlattenRanges(CompressSet(conjunctExtends), false));
    writer.Write(out);
}

// Conformance vectors: rows of NormalizationTest, and every row of GraphemeBreakTest.
//
// The normalization file is nineteen thousand rows, most of them one-code-point rows that exercise the same table
// entries; committing all of them would be weight without coverage. What is kept is every row of Part0 -- the
// curated specific cases, Hangul and reordering among them -- and a stride through the exhaustive parts that still
// crosses every algorithmic path a few hundred times.
void EmitConformance(const std::filesystem::path &normalizationPath, const std::filesystem::path &graphemePath,
                     const std::filesystem::path &out,
                     const std::vector<std::pair<std::string, std::string>> &hashes) {
    std::vector<std::uint32_t> index;
    std::vector<std::uint32_t> elements;
    std::ifstream in(normalizationPath);
    std::string line;
    int part = -1;
    std::size_t row = 0;
    std::size_t kept = 0;
    while (std::getline(in, line)) {
        if (line.starts_with("@Part")) {
            part = line[5] - '0';
            continue;
        }
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line = line.substr(0, comment);
        }
        if (Trim(line).empty()) {
            continue;
        }
        ++row;
        if (part == 1 && row % 13 != 0) {
            continue;
        }
        if (part >= 2 && row % 17 != 0) {
            continue;
        }
        const auto fields = SplitFields(line, ';');
        if (fields.size() < 5) {
            continue;
        }
        ++kept;
        for (int column = 0; column < 5; ++column) {
            const auto sequence = HexSequence(Trim(fields[column]));
            index.push_back(static_cast<std::uint32_t>(elements.size()));
            index.push_back(static_cast<std::uint32_t>(sequence.size()));
            elements.insert(elements.end(), sequence.begin(), sequence.end());
        }
    }
    std::printf("normalization vectors kept: %zu\n", kept);

    // Grapheme rows: the break marker is U+00F7 and the join marker U+00D7, in UTF-8. Encode each row as its code
    // points plus a parallel run of break flags, one per boundary position (before each point and after the last).
    std::vector<std::uint32_t> graphemeIndex;
    std::vector<std::uint32_t> graphemePoints;
    std::vector<std::uint32_t> graphemeBreaks;
    std::ifstream graphemes(graphemePath);
    std::size_t graphemeRows = 0;
    while (std::getline(graphemes, line)) {
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line = line.substr(0, comment);
        }
        if (Trim(line).empty()) {
            continue;
        }
        std::vector<std::uint32_t> points;
        std::vector<std::uint32_t> breaks;
        std::stringstream stream(line);
        std::string token;
        while (stream >> token) {
            if (token == "\xC3\xB7") {
                breaks.push_back(1);
            } else if (token == "\xC3\x97") {
                breaks.push_back(0);
            } else {
                points.push_back(Hex(token));
            }
        }
        if (breaks.size() != points.size() + 1) {
            continue;
        }
        graphemeIndex.push_back(static_cast<std::uint32_t>(graphemePoints.size()));
        graphemeIndex.push_back(static_cast<std::uint32_t>(points.size()));
        graphemePoints.insert(graphemePoints.end(), points.begin(), points.end());
        std::uint32_t packed = 0;
        for (std::size_t bit = 0; bit < breaks.size(); ++bit) {
            packed |= breaks[bit] << bit;
        }
        graphemeBreaks.push_back(packed);
        ++graphemeRows;
    }
    std::printf("grapheme vectors kept: %zu\n", graphemeRows);

    RuxWriter writer;
    writer.Header("The conformance vectors: normalization rows and every grapheme break row.", hashes);
    writer.Array("Normalization vector index: five (offset, length) pairs per row, for the columns c1..c5.",
                 "NormalizationVectorIndex", index);
    writer.Array("Normalization vector elements.", "NormalizationVectorElements", elements);
    writer.Array("Grapheme vector index: offset, length per row.", "GraphemeVectorIndex", graphemeIndex);
    writer.Array("Grapheme vector code points.", "GraphemeVectorPoints", graphemePoints);
    writer.Array("Grapheme vector break masks: bit N set when a boundary precedes point N, the highest bit for the "
                 "end of the row. No row is longer than thirty-one points.",
                 "GraphemeVectorBreaks", graphemeBreaks);
    writer.Write(out);
}

} // namespace

int main(int argc, char **argv) {
    const std::filesystem::path root = argc > 1 ? argv[1] : ".";
    const std::filesystem::path data = root / "Tools" / "UnicodeGen" / "Data";
    const std::filesystem::path target = root / "Packages" / "Unicode" / "Src";
    std::filesystem::create_directories(target);

    const auto hash = [&](const std::string &name) {
        return std::pair{name, FileSha256(data / name)};
    };

    Database db;
    ReadUnicodeData(db, data / "UnicodeData.txt");
    db.propList = ReadPropertyFile(data / "PropList.txt");
    db.coreProperties = ReadPropertyFile(data / "DerivedCoreProperties.txt");
    db.graphemeBreaks = ReadPropertyFile(data / "auxiliary/GraphemeBreakProperty.txt");
    db.emoji = ReadPropertyFile(data / "emoji/emoji-data.txt");
    ReadCompositionExclusions(db, data / "CompositionExclusions.txt");
    ReadCaseFolding(db, data / "CaseFolding.txt");
    ReadSpecialCasing(db, data / "SpecialCasing.txt");

    EmitProperties(db, target / "GeneratedProperties.rux",
                   {hash("UnicodeData.txt"), hash("PropList.txt"), hash("DerivedCoreProperties.txt")});
    EmitCase(db, target / "GeneratedCase.rux",
             {hash("UnicodeData.txt"), hash("CaseFolding.txt"), hash("SpecialCasing.txt")});
    EmitNormalization(db, target / "GeneratedNormalization.rux",
                      {hash("UnicodeData.txt"), hash("CompositionExclusions.txt")});
    EmitGraphemes(db, target / "GeneratedGraphemes.rux",
                  {hash("auxiliary/GraphemeBreakProperty.txt"), hash("emoji/emoji-data.txt"),
                   hash("DerivedCoreProperties.txt")});
    EmitConformance(data / "NormalizationTest.txt", data / "auxiliary/GraphemeBreakTest.txt",
                    target / "GeneratedConformance.rux",
                    {hash("NormalizationTest.txt"), hash("auxiliary/GraphemeBreakTest.txt")});
    return 0;
}
