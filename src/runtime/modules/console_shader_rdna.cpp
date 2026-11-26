#include "runtime/modules/console_shader_rdna.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <unordered_map>

// From the console GPU's machine code (AMD's RDNA instruction set, as LLVM's gfx1013 has it)
// to HLSL. The registers become variables of the shader, each instruction a statement on
// 32-bit patterns, and the shader is run for one lane of a wave: a mask of lanes is all
// ones or zero, and what the hardware does only for the lanes of EXEC is done if this
// lane's is set. Branches forward skip statements, branches back make loops.
//
// What a shader is given is in docs/research/console-shaders.md. The descriptors a shader
// loads are followed while it is translated, to the root signature's register they stand
// for; a shader that gets at one in a way that is not followed is given up on, as is one
// with an instruction that is not known here. `why` then names the first such thing.
namespace xv::runtime::modules::rdna {
namespace {
enum class F {
    sop2,
    sopk,
    sop1,
    sopc,
    sopp,
    smem,
    vop2,
    vop1,
    vopc,
    vop3,
    vop3p,
    vintrp,
    ds,
    flat,
    mubuf,
    mtbuf,
    mimg,
    exp,
    unknown
};
struct Op {
    F format;
    std::uint32_t op;
    const char *name;
};
// every instruction LLVM's disassembler names for this GPU (tools/rdna_ops.py)
constexpr Op ops[] = {
#include "runtime/modules/console_shader_rdna_ops.inc"
};
const char *name_of(F format, std::uint32_t op) {
    static const auto names = [] {
        std::unordered_map<std::uint32_t, const char *> table;
        for (const auto &known : ops)
            table.emplace(static_cast<std::uint32_t>(known.format) << 16 | known.op, known.name);
        return table;
    }();
    const auto found = names.find(static_cast<std::uint32_t>(format) << 16 | op);
    return found == names.end() ? nullptr : found->second;
}
// The Xbox One GPU's instructions (AMD's GCN, LLVM's "bonaire"; tools/gcn_ops.py): the same
// formats with other opcodes, some of them laid out another way. They are decoded into the
// Series form of the same instruction, which is what the rest of the translator reads.
constexpr Op older_ops[] = {
#include "runtime/modules/console_shader_gcn_ops.inc"
};
const char *older_name_of(F format, std::uint32_t op) {
    // Compares have the Series numbers, with signalling ones from 0x40 that are taken for
    // the quiet ones; the table has only those the assembler told of.
    if (format == F::vopc) {
        const auto quiet = op >= 0x40 && op < 0x80 ? op - 0x40 : op;
        const bool wide16 = (quiet & 0xf) > 8 && quiet >= 0x80; // 16-bit ones, newer
        return wide16 ? nullptr : name_of(F::vopc, quiet);
    }
    static const auto names = [] {
        std::unordered_map<std::uint32_t, const char *> table;
        for (const auto &known : older_ops)
            table.emplace(static_cast<std::uint32_t>(known.format) << 16 | known.op, known.name);
        return table;
    }();
    const auto found = names.find(static_cast<std::uint32_t>(format) << 16 | op);
    return found == names.end() ? nullptr : found->second;
}
// the opcode an instruction of this name has on the Series GPU in this format
std::optional<std::uint32_t> op_of(F format, std::string_view name) {
    static const auto by_name = [] {
        std::map<std::pair<F, std::string_view>, std::uint32_t> table;
        for (const auto &known : ops)
            table.emplace(std::pair{known.format, std::string_view(known.name)}, known.op);
        return table;
    }();
    const auto found = by_name.find({format, name});
    return found == by_name.end() ? std::nullopt : std::optional(found->second);
}
// What an older instruction is called now, or the one that stands for it: the legacy and
// clamped forms differ from the plain ones in what they make of NaN and infinity.
std::string_view newer_name(std::string_view name) {
    static constexpr std::pair<std::string_view, std::string_view> renamed[] = {
        {"v_add_i32", "v_add_co_u32"},       {"v_sub_i32", "v_sub_co_u32"},
        {"v_subrev_i32", "v_subrev_co_u32"}, {"v_addc_u32", "v_add_co_ci_u32"},
        {"v_subb_u32", "v_sub_co_ci_u32"},   {"v_subbrev_u32", "v_subrev_co_ci_u32"},
        {"v_min_legacy_f32", "v_min_f32"},   {"v_max_legacy_f32", "v_max_f32"},
        {"v_rcp_clamp_f32", "v_rcp_f32"},    {"v_rcp_legacy_f32", "v_rcp_f32"},
        {"v_rsq_clamp_f32", "v_rsq_f32"},    {"v_rsq_legacy_f32", "v_rsq_f32"},
        {"v_log_clamp_f32", "v_log_f32"},    {"v_mul_lo_i32", "v_mul_lo_u32"},
    };
    for (const auto &[older, newer] : renamed)
        if (name == older)
            return newer;
    return name;
}
// a vertex buffer format as the Series GPU numbers it, by the older data and number formats
constexpr std::uint8_t buffer_formats[128] = {
    0,  0,  0,  0,  0,  0,  0,  0,  1,  2,  3,  4,  5,  6,  0,  0,  7,  8,  9,  10, 11, 12,
    0,  13, 14, 15, 16, 17, 18, 19, 0,  0,  0,  0,  0,  0,  20, 21, 0,  22, 23, 24, 25, 26,
    27, 28, 0,  29, 30, 31, 32, 33, 34, 35, 0,  36, 37, 38, 39, 40, 41, 42, 0,  43, 44, 45,
    46, 47, 48, 49, 0,  0,  50, 51, 52, 53, 54, 55, 0,  0,  56, 57, 58, 59, 60, 61, 0,  0,
    0,  0,  0,  0,  62, 63, 0,  64, 65, 66, 67, 68, 69, 70, 0,  71, 0,  0,  0,  0,  72, 73,
    0,  74, 0,  0,  0,  0,  75, 76, 0,  77, 0,  0,  0,  0,  0,  0,  0,  0};

struct Unsupported {
    std::string what;
    std::uint32_t at = ~0u; // the instruction it was met at, in words from the first
    bool mismatch = false;  // a resource used as another kind than it was before
};
[[noreturn]] void give_up(std::string what) { throw Unsupported{std::move(what)}; }

std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t at) {
    std::uint32_t value = 0;
    if (at + 4 <= bytes.size())
        std::memcpy(&value, bytes.data() + at, 4);
    return value;
}

struct Range {
    std::uint32_t type, count, base, space, offset; // type: 0 SRV, 1 UAV, 2 CBV, 3 sampler
};
struct Parameter {
    std::uint32_t type = 0; // 0 table, 1 constants, 2 CBV, 3 SRV, 4 UAV
    std::uint32_t reg = 0, space = 0, count = 0;
    std::uint32_t visibility = 0; // 0 every stage, 1 vertex, 5 pixel
    std::vector<Range> ranges;
};
bool read_root_signature(std::span<const std::uint8_t> data, std::vector<Parameter> &parameters) {
    const auto version = word(data, 0), count = word(data, 4), at = word(data, 8);
    if ((version != 1 && version != 2) || count > 64 || at + 12ull * count > data.size())
        return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        Parameter parameter;
        parameter.type = word(data, at + 12 * i);
        parameter.visibility = word(data, at + 12 * i + 4);
        const auto offset = word(data, at + 12 * i + 8);
        if (parameter.type == 0) {
            const auto ranges = word(data, offset), start = word(data, offset + 4);
            const std::size_t size = version == 2 ? 24 : 20;
            if (ranges > 4096 || start + size * ranges > data.size())
                return false;
            std::uint32_t append = 0;
            for (std::uint32_t r = 0; r < ranges; ++r) {
                const auto entry = start + size * r;
                Range range{word(data, entry), word(data, entry + 4), word(data, entry + 8),
                            word(data, entry + 12), word(data, entry + size - 4)};
                if (range.offset == ~0u)
                    range.offset = append;
                append = range.count == ~0u ? range.offset : range.offset + range.count;
                parameter.ranges.push_back(range);
            }
        } else if (parameter.type == 1) {
            parameter.reg = word(data, offset);
            parameter.space = word(data, offset + 4);
            parameter.count = word(data, offset + 8);
        } else if (parameter.type <= 4) {
            parameter.reg = word(data, offset);
            parameter.space = word(data, offset + 4);
        } else {
            return false;
        }
        parameters.push_back(std::move(parameter));
    }
    return true;
}

// one instruction, with the fields of its format
struct Ins {
    F format = F::unknown;
    std::uint32_t op = 0;
    std::string_view name;
    std::uint32_t at = 0, words = 1; // where it is, in words from the first instruction
    std::uint32_t w0 = 0, w1 = 0, literal = 0;
    std::uint32_t extra[3] = {}; // more address registers of an image instruction
    // The word after an instruction whose first source is not a register but says that
    // parts of registers are used (sub_dword) or another lane's (lanes).
    enum class Form { plain, sub_dword, lanes } form = Form::plain;
    std::uint32_t modifier = 0;
};
bool is(const Ins &ins, std::string_view name) { return ins.name == name; }
bool starts(std::string_view text, std::string_view with) {
    return text.substr(0, with.size()) == with;
}
bool ends(std::string_view text, std::string_view with) {
    return text.size() >= with.size() && text.substr(text.size() - with.size()) == with;
}

// what is known of a scalar register while translating, besides its value at run time
struct Known {
    // table: the address of a descriptor table; descriptor: a word of a descriptor loaded
    // from one; root: half of the address a root descriptor gives; code: an address in the
    // shader's own code
    // conflict: one thing or another, by the way the shader came
    // lanes: a mask with a bit for each lane of the wave
    // memory: the address of the root parameters an Xbox One shader has no registers for
    enum Kind {
        nothing,
        constant,
        table,
        descriptor,
        root,
        code,
        conflict,
        lanes,
        memory,
        streams, // the address of the vertex streams' buffer descriptors (a cached program)
        stream   // a word of one of those descriptors; value: which stream
    } kind = nothing;
    std::uint32_t value = 0;            // the constant; the root parameter of the others
    std::uint32_t offset = 0, part = 0; // a descriptor: its byte offset in the table, and
                                        // which of its words this is; a root: which half
    int dynamic = -1;                   // a descriptor whose offset is only known at run time: the
                                        // variable that has it
    std::uint32_t origin = 0;           // the instruction that loaded the descriptor
    std::uint32_t stride = 0;           // the upper half of a root address made into a buffer
                                        // descriptor: the stride given to it
    std::uint32_t choice = 0;           // a descriptor whose register holds another on other
                                        // ways through the shader: the variable that says
                                        // which was loaded, plus one
    bool several = false;               // and here it is not known which
    bool maybe = false;                 // only on some of the ways that lead here
    bool operator==(const Known &) const = default;
};
// a register that stands for something of the console's, and has no value of its own here
bool stands_in(const Known &known) {
    return known.kind == Known::table || known.kind == Known::descriptor ||
           known.kind == Known::root || known.kind == Known::memory ||
           known.kind == Known::streams || known.kind == Known::stream;
}
// What is known where two ways through a shader meet. A descriptor that only one of them
// loads stays one: compiled code loads descriptors in blocks that are skipped when no lane
// is active, and uses them after.
Known join(const Known &a, const Known &b) {
    if (a.kind == Known::conflict || b.kind == Known::conflict)
        return {Known::conflict};
    if (a.kind == Known::descriptor && b.kind == Known::descriptor && a.choice &&
        a.choice == b.choice && a.part == b.part &&
        (a.origin != b.origin || a.several != b.several)) {
        Known either = a;
        either.origin = a.choice - 1;
        either.offset = 0;
        either.several = true;
        either.maybe = a.maybe || b.maybe;
        return either;
    }
    if (stands_in(a) && stands_in(b)) {
        auto both = a;
        both.maybe = b.maybe;
        if (!(both == b))
            return {Known::conflict};
        both.maybe = a.maybe || b.maybe;
        return both;
    }
    if (stands_in(a) || stands_in(b)) {
        auto one = stands_in(a) ? a : b;
        one.maybe = true;
        return one;
    }
    return a == b ? a : Known{};
}
// the scalar registers, then the places a shader keeps scalars in lanes of vector registers
constexpr std::size_t known_slots = 128 + 96;
using State = std::array<Known, known_slots>;
struct Resource {
    std::uint32_t type = 0, reg = 0, space = 0; // type as a Range's
    bool root = false;                          // a root descriptor: an address, not a view
    std::uint32_t stride = 0;                   // of a root buffer, if the shader says
    int dynamic = -1; // one of an array: the variable with its place in the table
    std::uint32_t parameter = 0, origin = 0;
    bool loaded = false; // from a table, by the instruction `origin`
    bool stream = false; // a vertex stream, `reg` its number: read as the shader's inputs
    bool heap = false;   // read from the heap itself, `reg` descriptors after the table's first
};
// What a first pass over a shader found out that changes how it is translated: the
// descriptors written through, which are unordered access views where the table allows both.
struct Facts {
    std::set<std::uint32_t> written;
    std::map<std::uint32_t, std::string> types; // what each loaded descriptor is used as
    // Loads whose descriptors meet in one register where ways through the shader join, each
    // by the first load of its set: one variable then says which was loaded.
    std::map<std::uint32_t, std::uint32_t> met;
    // Set once a pass has seen what each of them is used as where that is certain: until
    // then an instruction is not made for a descriptor it only may have been given.
    bool learned = false;
    std::uint32_t first_of(std::uint32_t origin) const {
        for (auto found = met.find(origin); found != met.end() && found->second != origin;
             found = met.find(origin))
            origin = found->second;
        return origin;
    }
    bool meet(std::uint32_t a, std::uint32_t b) {
        a = first_of(a), b = first_of(b);
        if (a == b)
            return false;
        met[std::max(a, b)] = std::min(a, b);
        met.try_emplace(std::min(a, b), std::min(a, b));
        return true;
    }
};
struct Retry {};
// thrown where an instruction uses a descriptor that is one of several
struct Choose {
    std::uint32_t first;
};

class Translator {
  public:
    Translator(Stage stage, const Shader &shader, Facts &facts)
        : stage_(stage), shader_(shader), facts_(facts) {}

    bool needs_sm6() const { return needs_sm6_; }

    std::string run() {
        // one that reads the heaps itself has what it needs in its own file
        if (!read_root_signature(shader_.root_signature, parameters_) &&
            !(shader_.heaps && shader_.root_signature.empty()))
            give_up("root signature");
        read_file();
        decode();
        find_blocks();
        analyse();
        prepare_registers();
        // an Xbox One vertex shader with nothing to fetch calls no fetch shader
        if (older_ && stage_ == Stage::vertex && code_list_[0].name != "s_swappc_b64")
            fetch_inputs();
        // A Series vertex shader that takes nothing but the numbers of its vertex and its
        // instance calls no fetch shader either: it reads its vertices from buffers itself,
        // by the vertex's number in v0 (`v1 = s5 * v0`).
        if (!older_ && stage_ == Stage::vertex &&
            std::none_of(code_list_.begin(), code_list_.end(),
                         [](const Ins &ins) { return ins.name == "s_swappc_b64"; }) &&
            std::all_of(shader_.inputs.begin(), shader_.inputs.end(), generated))
            fetch_inputs();
        // a cached pipeline's vertex program: the vertex's number is in v0, and the rest it
        // reads from its streams
        if (shader_.streams && stage_ == Stage::vertex) {
            for (std::size_t i = 0; i < shader_.inputs.size(); ++i)
                if (!_stricmp(shader_.inputs[i].name.c_str(), "SV_VertexID")) {
                    line(v(0) + " = input.f" + std::to_string(i) + ";");
                    vertex_id_ = true;
                }
            fetched_ = true;
        }
        for (std::size_t block = 0; block < blocks_.size(); ++block)
            emit_block(block);
        // a descriptor whose words the shader reads, met before what it is was known
        for (const auto origin : wanted_) {
            if (facts_.types.count(origin))
                throw Retry{};
            give_up("a descriptor used as a value");
        }
        if (unlearned_ && !facts_.learned) {
            facts_.learned = true;
            throw Retry{};
        }
        return finish();
    }

  private:
    // the first user register of a shader that reads the heaps itself, which is xv_ud0: a
    // compute shader's start at s0, the other stages' after the eight the hardware sets
    std::uint32_t user_first() const { return stage_ == Stage::compute ? 0u : 8u; }
    // A mesh shader runs as the merged stages do, with eight registers of the hardware's
    // first: what its file says is user register n is s(n + 8).
    std::uint32_t user_offset() const { return stage_ == Stage::mesh ? 8u : 0u; }
    // An Xbox One shader: its size, 5 at 0x18, where its code is at 0x28; the program starts
    // 0x28 into the code and is as long as 0x1dc says.
    bool read_older_file() {
        const auto file = shader_.file;
        if (file.size() < 0x1e0 || word(file, 0) != file.size() || word(file, 0x18) != 5)
            return false;
        const auto start = word(file, 0x28) + 0x28, size = word(file, 0x1dc);
        if (start > file.size() || size > file.size() - start || size % 4)
            give_up("no code");
        older_ = true;
        for (std::uint32_t at = start; at + 4 <= start + size; at += 4)
            code_.push_back(word(file, at));
        // before the program, COMPUTE_PGM_RSRC2 and the group size
        header_[0] = word(file, start - 0x18);
        for (int i = 0; i < 3; ++i)
            header_[1 + i] = word(file, start - 0x10 + 4 * i);
        // The records after the program, five words each. Those of kind 0x1a say where a
        // root parameter is: its number, its words, and the register it starts in, or from
        // 16 its place in the memory the first register has the address of.
        known_[0] = {Known::memory};
        std::set<std::uint32_t> seen;
        for (std::size_t at = start + size;
             at + 20 <= file.size() && (word(file, at) || word(file, at + 4)); at += 20) {
            if ((word(file, at) & 0xff) != 0x1a)
                continue;
            const auto where = word(file, at + 4);
            const auto p = where & 0xff, words = (where >> 8) & 0xff, place = (where >> 16) & 0xff;
            if (p >= parameters_.size() || !seen.insert(p).second)
                continue;
            const auto &parameter = parameters_[p];
            if (words != (parameter.type == 0 ? 1 : parameter.type == 1 ? parameter.count : 2))
                give_up("a root parameter of another size than its record says");
            if (place >= 16) {
                in_memory_.push_back({place - 16, words, p});
            } else if (parameter.type == 0) {
                known_[place] = {Known::table, p};
            } else if (parameter.type == 1) {
                in_registers_.push_back({place, words, p});
            } else {
                known_[place] = {Known::root, p};
                known_[place + 1] = {Known::root, p, 0, 1};
            }
        }
        return true;
    }
    // where the words of a root parameter are: from a register on, or in memory
    struct Placed {
        std::uint32_t at, words, parameter;
    };
    const Placed *in_memory(std::uint32_t word) const {
        for (const auto &placed : in_memory_)
            if (word >= placed.at && word - placed.at < placed.words)
                return &placed;
        return nullptr;
    }
    // a word of root constants: of the constant buffer the stage sees them as
    std::string root_constant(std::uint32_t parameter, std::uint32_t word) {
        const auto &constants = parameters_[parameter];
        const auto mine = stage_ == Stage::vertex ? 1u : stage_ == Stage::pixel ? 5u : 0u;
        if (constants.visibility && constants.visibility != mine)
            return "0u";
        const auto name = this->constants(Resource{2, constants.reg, constants.space});
        auto &size = constants_size_[name];
        size = std::max(size, word / 4 + 1);
        return name + "[" + std::to_string(word / 4) + "][" + std::to_string(word & 3) + "]";
    }
    void read_file() {
        const auto file = shader_.file;
        if (shader_.streams)
            known_[0] = known_[1] = {Known::streams};
        if (read_older_file())
            return;
        if (file.size() < 0x140 || std::memcmp(file.data(), "FSSM", 4))
            give_up("no code");
        const auto region = word(file, 0xc0), size = word(file, 0xc8);
        if (region > file.size() || size > file.size() - region || size < 0x2c)
            give_up("no code");
        // COMPUTE_PGM_RSRC2 and the group size: at the region (file word 0x10 == 5) or two
        // words before it (6)
        const auto header = word(file, 0x10) >= 6 && region >= 8 ? region - 8 : region;
        for (int i = 0; i < 4; ++i)
            header_[i] = word(file, header + 4 * i);
        for (std::uint32_t at = region + 0x28; at + 4 <= region + size; at += 4)
            code_.push_back(word(file, at));
        // A mesh shader's record (at 0xc0, as the other stages') is followed by its stage
        // data: from 0xc8 on its outputs, 6 bytes each {usage, row, parameter (bit 7: one of
        // the primitive's), ...}, as many as 0x188 says; at 0x448 the lanes of its group, its
        // most vertices and primitives, its user registers and its topology (2 triangles).
        // The record's flags at 0x14 say wave32 by bit 5; its LDS bytes are at 0x4c.
        if (stage_ == Stage::mesh) {
            constexpr std::size_t record = 0xc0, data = record + 0x54;
            if (data + 0x450 > file.size())
                give_up("no mesh stage data");
            const auto half = [&](std::size_t at) {
                std::uint16_t value = 0;
                std::memcpy(&value, file.data() + at, 2);
                return std::uint32_t{value};
            };
            mesh_threads_ = half(data + 0x448);
            mesh_vertices_ = half(data + 0x44a);
            mesh_primitives_ = half(data + 0x44c);
            mesh_wave_ = word(file, record + 0x14) >> 5 & 1 ? 32 : 64;
            mesh_shared_ = word(file, record + 0x4c);
            if (file[data + 0x44f] != 2)
                give_up("a mesh shader not drawing triangles");
            if (!mesh_threads_ || mesh_threads_ > 128 || mesh_vertices_ > 256 ||
                mesh_primitives_ > 256 || !mesh_vertices_ || !mesh_primitives_)
                give_up("a mesh shader beyond the host's limits");
            for (std::uint32_t n = 0; n < std::min(word(file, data + 0x188), 32u); ++n) {
                const auto at = data + 0xc8 + 6 * n;
                mesh_outputs_.push_back({file[at + 2], file[at + 1]});
            }
        }
        // the records after the code: where the root parameters are
        std::size_t at = region + size;
        while (word(file, at) == 0xbf9f0000)
            at += 4;
        bool any = false;
        const auto root = [&](std::uint32_t reg, std::uint32_t parameter) {
            known_[reg] = {Known::root, parameter};
            known_[reg + 1] = {Known::root, parameter, 0, 1};
        };
        bound_.assign(shader_.bound.begin(), shader_.bound.end());
        // A shader that reads the heaps itself (see rdna::Shader::heaps), with its tables
        // after the code: `layout` (as many as the word at 0xf8 says) has what each user
        // register holds - {type, place in the table in words, registers, shader register,
        // u8 first register, u8 root parameter}, type 0 constants, 1 a root view, 3 a table,
        // 4 a sampler of the signature's, 5 and 6 the addresses of the heaps - and `used`
        // (0xfc) what is read at each place of a table: {kind, place in words, shader
        // register, count, u8 words, u8 the table's register}.
        const auto layouts = word(file, 0xf8), reads = word(file, 0xfc);
        if (shader_.heaps && layouts && layouts < 4096 && reads < 4096 &&
            at + 20ull * (layouts + reads) <= file.size()) {
            for (std::uint32_t n = 0; n < layouts; ++n) {
                const auto entry = at + 20 * n;
                const auto type = word(file, entry), count = word(file, entry + 8);
                const std::uint32_t reg = file[entry + 16] + user_offset(), slot = file[entry + 17];
                if (type == 4)
                    continue;
                if (reg < user_first() || reg + (type == 0   ? count
                                                 : type == 1 ? 2
                                                             : 1) >
                                              user_first() + 24)
                    give_up("user registers the host's shader is not given");
                if (type == 1) {
                    // A root view: the host's signature has one of each kind in space 201,
                    // which gets the address the title gives this parameter. It is a shader
                    // resource unless a record says the program reads it as another kind.
                    Parameter view;
                    view.type = 3;
                    view.space = 201;
                    for (std::uint32_t k = 0; k < reads; ++k) {
                        const auto read = at + 20 * (layouts + k);
                        if (file[read + 1] == 1 && file[read + 18] == slot)
                            view.type = file[read] == 2 ? 2u : file[read] == 3 ? 4u : 3u;
                    }
                    std::uint32_t tables = 0;
                    for (std::uint32_t k = 0; k < layouts; ++k)
                        if (word(file, at + 20 * k) == 3)
                            tables = std::max<std::uint32_t>(tables, file[at + 20 * k + 17] + 1);
                    if (parameters_.size() < tables)
                        parameters_.resize(tables);
                    root(reg, static_cast<std::uint32_t>(parameters_.size()));
                    parameters_.push_back(view);
                    continue;
                }
                if (type == 3) {
                    if (slot >= parameters_.size())
                        parameters_.resize(slot + 1);
                    table_register_[slot] = reg;
                    known_[reg] = {Known::table, slot};
                } else if (type == 0) {
                    for (std::uint32_t k = 0; k < count; ++k) {
                        used_s_.insert(reg + k);
                        uses_heap_ = true;
                        prologue_ += "    " + s(reg + k) + " = xv_ud" +
                                     std::to_string(reg + k - user_first()) + ";\n";
                    }
                } else if (type == 5 || type == 6) {
                    const auto whole = static_cast<std::uint32_t>(parameters_.size());
                    heap_parameter_[whole] = type == 5 ? 1 : 2;
                    known_[reg] = {Known::table, whole};
                    parameters_.emplace_back();
                }
            }
            for (std::uint32_t n = 0; n < reads; ++n) {
                const auto entry = at + 20 * (layouts + n);
                const auto kind = word(file, entry);
                const auto type = kind == 0x300   ? 0u
                                  : kind == 0x303 ? 1u
                                  : kind == 0x302 ? 2u
                                  : kind == 2     ? 3u
                                                  : ~0u;
                if (type != ~0u)
                    bound_.push_back(
                        {word(file, entry + 4) * 4, type, file[entry + 17] + user_offset()});
            }
            any = true;
            at = file.size();
        }
        for (; at + 20 <= file.size() && word(file, at + 8) != ~0u; at += 20) {
            const auto kind = word(file, at);
            if (kind != 3 && kind != 1)
                continue;
            const auto where = word(file, at + 16), parameter = (where >> 8) & 0xff,
                       reg = (where & 0xff) + user_offset();
            if (parameter >= parameters_.size() || reg >= 105)
                continue;
            if (kind == 3 && parameters_[parameter].type == 0)
                known_[reg] = {Known::table, parameter}, any = true;
            else if (kind == 1 && parameters_[parameter].type >= 2)
                root(reg, parameter), any = true;
        }
        if (!any) { // no records: the parameters in order, from the first user register
            std::uint32_t reg = stage_ == Stage::compute ? 4 : 8;
            const auto real = parameters_.size();
            for (std::uint32_t p = 0; p < real && reg < 100; ++p) {
                if (parameters_[p].type == 0) {
                    table_register_[p] = reg;
                    known_[reg++] = {Known::table, p};
                } else if (parameters_[p].type == 1) {
                    // a cached program's: the constants as the host's root constants have them
                    for (std::uint32_t n = 0; shader_.heaps && n < parameters_[p].count; ++n) {
                        used_s_.insert(reg + n);
                        uses_heap_ = true;
                        prologue_ +=
                            "    " + s(reg + n) + " = xv_ud" + std::to_string(reg + n - 8) + ";\n";
                    }
                    reg += parameters_[p].count;
                } else {
                    root(reg, p);
                    reg += 2;
                }
            }
        }
        // A cached program: after the parameters, the address of each heap its signature
        // lets it index (flags 0x400 the views, 0x800 the samplers), each as a table of
        // its own that has all of the heap.
        if (shader_.heaps && !any) {
            std::uint32_t reg = 8;
            for (const auto &parameter : parameters_)
                reg += parameter.type == 0 ? 1 : parameter.type == 1 ? parameter.count : 2;
            const auto flags = word(shader_.root_signature, 20);
            for (const std::uint32_t kind : {1u, 2u})
                if (flags & (0x200u << kind) && reg < 100) {
                    heap_parameter_[static_cast<std::uint32_t>(parameters_.size())] = kind;
                    known_[reg++] = {Known::table, static_cast<std::uint32_t>(parameters_.size())};
                    parameters_.emplace_back();
                }
        }
        // a pixel shader: which register of its input signature each attribute is
        if (stage_ == Stage::pixel)
            for (std::size_t entry = 0x130; entry + 8 <= 0x130 + 8 * 32 && file[entry]; entry += 8)
                attributes_[file[entry + 3]] = file[entry + 1];
    }

    // What is decoded is what the first instruction leads to: constants a shader reads
    // through its program counter lie between and after its instructions, and are not code.
    void decode() {
        std::map<std::uint32_t, Ins> found;
        std::vector<std::uint32_t> work{0};
        while (!work.empty()) {
            auto at = work.back();
            work.pop_back();
            while (at < code_.size() && !found.count(at)) {
                const auto ins = decode_at(at);
                found.emplace(at, ins);
                if (ins.name == "s_endpgm" || ins.name == "s_code_end")
                    break;
                if (ins.name == "s_setpc_b64" || ins.name == "s_rfe_b64")
                    give_up("a jump to an address in a register");
                if (branch(ins)) {
                    const auto target = target_of(ins);
                    if (target >= code_.size())
                        give_up("a branch out of the code");
                    work.push_back(target);
                    if (ins.name == "s_branch")
                        break;
                }
                at += ins.words;
            }
        }
        for (const auto &[at, ins] : found) {
            if (!code_list_.empty() && code_list_.back().at + code_list_.back().words > at)
                give_up("branch into the middle of an instruction");
            index_of_[at] = code_list_.size();
            code_list_.push_back(ins);
        }
        if (code_list_.empty())
            give_up("no code");
        data_first_ = code_list_.back().at + code_list_.back().words;
        find_modes();
    }
    // One instruction of the older GPU, as the Series instruction it is: same place and
    // length in the code, the fields where the Series format has them.
    void decode_older(Ins &ins) {
        const auto at = ins.at;
        const auto w = code_[at];
        const auto top = w >> 26;
        const auto unknown = [&] {
            char text[48];
            std::snprintf(text, sizeof text, "older instruction %08x", w);
            give_up(text);
        };
        const auto second = [&] {
            if (at + 1 >= code_.size())
                give_up("truncated code");
            ins.words = 2;
            return code_[at + 1];
        };
        // the same instruction with the Series opcode, where only that differs
        const auto same = [&](F format, const char *name, std::uint32_t shift, std::uint32_t mask) {
            if (!name)
                unknown();
            const auto op = op_of(format, newer_name(name));
            if (!op)
                give_up(std::string("older instruction ") + name);
            ins.format = format, ins.op = *op;
            ins.w0 = (w & ~(mask << shift)) | (*op & mask) << shift;
        };
        // as a three-operand instruction, which takes any source in any place
        const auto wide = [&](std::string_view name, std::uint32_t destination, std::uint32_t abs,
                              bool clamp, std::uint32_t sources, std::uint32_t rest) {
            std::uint32_t a = sources & 0x1ff, b = (sources >> 9) & 0x1ff;
            for (const auto &[plain, reversed] :
                 {std::pair<std::string_view, std::string_view>{"v_lshr_b32", "v_lshrrev_b32"},
                  {"v_ashr_i32", "v_ashrrev_i32"},
                  {"v_lshl_b32", "v_lshlrev_b32"},
                  {"v_lshr_b64", "v_lshrrev_b64"},
                  {"v_ashr_i64", "v_ashrrev_i64"},
                  {"v_lshl_b64", "v_lshlrev_b64"}})
                if (name == plain)
                    name = reversed, std::swap(a, b);
            const auto newer = newer_name(name);
            const auto op = op_of(F::vop3, newer);
            if (!op)
                give_up("older instruction " + std::string(name));
            // the carry is VCC where the instruction has no place to name it
            if (newer.find("_co_") != std::string_view::npos && !abs)
                abs = 106;
            if (newer.find("_ci_") != std::string_view::npos && !(rest >> 18 & 0x1ff))
                rest |= 106u << 18;
            ins.format = F::vop3, ins.op = *op;
            ins.w0 = 0x35u << 26 | *op << 16 | (clamp ? 1u << 15 : 0) | abs << 8 | destination;
            ins.w1 = a | b << 9 | rest;
        };
        if (top == 0x34) { // VOP3: opcode in bits 17 to 25, clamp in bit 11
            const auto op = (w >> 17) & 0x1ff;
            const auto *name = older_name_of(F::vop3, op);
            if (!name)
                name = op < 0x100    ? older_name_of(F::vopc, op)
                       : op < 0x140  ? older_name_of(F::vop2, op - 0x100)
                       : op >= 0x180 ? older_name_of(F::vop1, op - 0x180)
                                     : nullptr;
            if (!name)
                unknown();
            const auto next = second();
            // a compare has the register its result goes to where the others have abs
            const bool compare = op < 0x100;
            const bool carries = op >= 0x125 && op <= 0x12a;
            wide(name, w & 0xff, compare ? 0 : (w >> 8) & (carries ? 0x7f : 7),
                 !compare && !carries && (w >> 11 & 1), next & 0x3ffff, next & ~0x3ffffu);
        } else if (top == 0x32) {
            same(F::vintrp, older_name_of(F::vintrp, (w >> 16) & 3), 16, 3);
        } else if (top == 0x36) {
            ins.w1 = second();
            same(F::ds, older_name_of(F::ds, (w >> 18) & 0xff), 18, 0xff);
        } else if (top == 0x38) {
            ins.w1 = second();
            if (w >> 15 & 1)
                give_up("a buffer read by address");
            same(F::mubuf, older_name_of(F::mubuf, (w >> 18) & 0x7f), 18, 0x7f);
        } else if (top == 0x3a) { // the format is two fields there, one number here
            ins.w1 = second() & ~(1u << 21);
            const auto *const name = older_name_of(F::mtbuf, (w >> 16) & 7);
            const auto op = name ? op_of(F::mtbuf, name) : std::nullopt;
            if (!op)
                unknown();
            ins.format = F::mtbuf, ins.op = *op;
            ins.w0 = (w & 0xfc003fff) | (*op & 7) << 16 |
                     std::uint32_t{buffer_formats[(w >> 19) & 0x7f]} << 19;
            ins.w1 |= (*op >> 3) << 21;
        } else if (top == 0x3c) { // no dimension there: an array or not
            ins.w1 = second();
            const auto *const name = older_name_of(F::mimg, (w >> 18) & 0x7f);
            const auto op = name ? op_of(F::mimg, name) : std::nullopt;
            if (!op)
                unknown();
            ins.format = F::mimg, ins.op = *op;
            ins.w0 =
                (w & 0xfe03ff00) | (*op & 0x7f) << 18 | (*op >> 7) | (w >> 14 & 1 ? 5u : 1u) << 3;
        } else if (top == 0x3e) {
            ins.format = F::exp, ins.op = 0, ins.w0 = w, ins.w1 = second();
        } else if (w >> 27 == 0x18) { // SMRD: one word, the offset in it and in words
            const auto *const name = older_name_of(F::smem, (w >> 22) & 0x1f);
            const auto op = name ? op_of(F::smem, name) : std::nullopt;
            if (!op)
                unknown();
            const auto data = (w >> 15) & 0x7f, base = (w >> 9) & 0x3f, offset = w & 0xff;
            ins.format = F::smem, ins.op = *op;
            ins.w0 = 0x3du << 26 | *op << 18 | data << 6 | base;
            if (w >> 8 & 1)
                ins.w1 = 0x7du << 25 | offset * 4;
            else if (offset == 0xff) // in the next word, and in words as well
                ins.w1 = 0x7du << 25 | (second() * 4 & 0x1fffff);
            else // in a register, in bytes
                ins.w1 = offset << 25;
        } else if (w >> 23 == 0x17d) {
            same(F::sop1, older_name_of(F::sop1, (w >> 8) & 0xff), 8, 0xff);
        } else if (w >> 23 == 0x17e) {
            same(F::sopc, older_name_of(F::sopc, (w >> 16) & 0x7f), 16, 0x7f);
        } else if (w >> 23 == 0x17f) {
            same(F::sopp, older_name_of(F::sopp, (w >> 16) & 0x7f), 16, 0x7f);
        } else if (w >> 28 == 0xb) {
            // 0x15 sets a hardware register from the next word; the assembler has no name for it
            const auto op = (w >> 23) & 0x1f;
            same(F::sopk, op == 0x15 ? "s_setreg_imm32_b32" : older_name_of(F::sopk, op), 23, 0x1f);
        } else if (w >> 30 == 2) {
            same(F::sop2, older_name_of(F::sop2, (w >> 23) & 0x7f), 23, 0x7f);
        } else if (w >> 25 == 0x3f) {
            same(F::vop1, older_name_of(F::vop1, (w >> 9) & 0xff), 9, 0xff);
        } else if (w >> 25 == 0x3e) {
            same(F::vopc, older_name_of(F::vopc, (w >> 17) & 0xff), 17, 0xff);
        } else if (w >> 31 == 0) {
            const auto *const name = older_name_of(F::vop2, (w >> 25) & 0x3f);
            if (!name)
                unknown();
            if (op_of(F::vop2, newer_name(name)))
                same(F::vop2, name, 25, 0x3f);
            else
                wide(name, (w >> 17) & 0xff, 0, false, (w & 0x1ff) | (256 + ((w >> 9) & 0xff)) << 9,
                     0);
        } else {
            unknown();
        }
    }
    Ins decode_at(std::uint32_t at) {
        Ins ins;
        ins.at = at;
        auto w = ins.w0 = code_[at];
        const auto top = w >> 26;
        const auto second = [&] {
            if (at + 1 >= code_.size())
                give_up("truncated code");
            ins.w1 = code_[at + 1];
            ins.words = 2;
        };
        if (older_)
            decode_older(ins), w = ins.w0;
        else if (top == 0x33)
            ins.format = F::vop3p, ins.op = (w >> 16) & 0x7f, second();
        else if (top == 0x35)
            ins.format = F::vop3, ins.op = (w >> 16) & 0x3ff, second();
        else if (top == 0x32)
            ins.format = F::vintrp, ins.op = (w >> 16) & 3;
        else if (top == 0x36)
            ins.format = F::ds, ins.op = (w >> 18) & 0xff, second();
        else if (top == 0x37)
            ins.format = F::flat, ins.op = (w >> 18) & 0x7f, second();
        else if (top == 0x38)
            ins.format = F::mubuf, ins.op = (w >> 18) & 0x7f, second();
        else if (top == 0x3a)
            second(), ins.format = F::mtbuf, ins.op = ((w >> 16) & 7) | ((ins.w1 >> 21) & 1) << 3;
        else if (top == 0x3c)
            ins.format = F::mimg, ins.op = (w & 1) << 7 | ((w >> 18) & 0x7f), second();
        else if (top == 0x3d)
            ins.format = F::smem, ins.op = (w >> 18) & 0xff, second();
        else if (top == 0x3e)
            ins.format = F::exp, ins.op = 0, second();
        else if ((w >> 23) == 0x17d)
            ins.format = F::sop1, ins.op = (w >> 8) & 0xff;
        else if ((w >> 23) == 0x17e)
            ins.format = F::sopc, ins.op = (w >> 16) & 0x7f;
        else if ((w >> 23) == 0x17f)
            ins.format = F::sopp, ins.op = (w >> 16) & 0x7f;
        else if ((w >> 28) == 0xb)
            ins.format = F::sopk, ins.op = (w >> 23) & 0x1f;
        else if ((w >> 30) == 2)
            ins.format = F::sop2, ins.op = (w >> 23) & 0x7f;
        else if ((w >> 25) == 0x3f)
            ins.format = F::vop1, ins.op = (w >> 9) & 0xff;
        else if ((w >> 25) == 0x3e)
            ins.format = F::vopc, ins.op = (w >> 17) & 0xff;
        else if ((w >> 31) == 0)
            ins.format = F::vop2, ins.op = (w >> 25) & 0x3f;
        const auto *const name = name_of(ins.format, ins.op);
        if (!name) {
            char text[48];
            std::snprintf(text, sizeof text, "instruction %08x", w);
            give_up(text);
        }
        ins.name = name;
        const auto more = [&] {
            if (at + ins.words >= code_.size())
                give_up("truncated code");
            return code_[at + ins.words++];
        };
        // more words: the registers of an image instruction's address, or a literal
        if (ins.format == F::mimg && !older_)
            for (std::uint32_t n = 0, count = (w >> 1) & 3; n < count; ++n)
                ins.extra[n] = more();
        if (ins.format == F::vop1 || ins.format == F::vop2 || ins.format == F::vopc) {
            const auto source = w & 0x1ff;
            if (source == 249)
                ins.form = Ins::Form::sub_dword, ins.modifier = more();
            else if (source == 250 || source == 233 || source == 234)
                ins.form = Ins::Form::lanes, ins.modifier = more();
        }
        bool literal = false;
        const auto uses = [&](std::uint32_t source) { literal |= source == 255; };
        switch (ins.format) {
        case F::sop2:
            uses(w & 0xff), uses((w >> 8) & 0xff);
            break;
        case F::sop1:
        case F::sopc:
            uses(w & 0xff);
            if (ins.format == F::sopc)
                uses((w >> 8) & 0xff);
            break;
        case F::vop2:
            uses(w & 0x1ff);
            literal |= ins.name == "v_fmaak_f32" || ins.name == "v_fmamk_f32" ||
                       ins.name == "v_madak_f32" || ins.name == "v_madmk_f32" ||
                       ins.name == "v_fmaak_f16" || ins.name == "v_fmamk_f16";
            break;
        case F::vop1:
        case F::vopc:
            uses(w & 0x1ff);
            break;
        case F::vop3:
        case F::vop3p:
            uses(ins.w1 & 0x1ff), uses((ins.w1 >> 9) & 0x1ff), uses((ins.w1 >> 18) & 0x1ff);
            break;
        case F::sopk:
            literal = ins.name == "s_setreg_imm32_b32";
            break;
        default:
            break;
        }
        if (literal)
            ins.literal = more();
        return ins;
    }
    void find_modes() {
        for (const auto &ins : code_list_) {
            if (ins.format == F::sopp || ins.format == F::sop1 || ins.format == F::sop2) {
                const auto destination = (ins.w0 >> 16) & 0x7f;
                if ((ins.format != F::sopp && (destination == 126 || destination == 127)) ||
                    ins.name.find("saveexec") != std::string_view::npos)
                    exec_changes_ = true;
            }
            if (starts(ins.name, "v_cmpx"))
                exec_changes_ = true;
            if (ins.format == F::sopk && ((ins.w0 >> 16) & 0x7f) >= 126 &&
                (ins.name == "s_movk_i32" || ins.name == "s_cmovk_i32" ||
                 ins.name == "s_addk_i32" || ins.name == "s_mulk_i32"))
                sopk_writes_exec_ = true;
            if (ends(ins.name, "_b64") || ends(ins.name, "saveexec_b64"))
                wave64_ = true;
            if (ins.format == F::ds && ins.name != "ds_swizzle_b32" && ins.name != "ds_nop")
                uses_shared_ = true;
            if (stage_ == Stage::compute &&
                (ins.form == Ins::Form::lanes || starts(ins.name, "v_permlane")))
                sums_lanes_ = true;
        }
    }

    struct Block {
        std::size_t first, end;          // instructions [first, end)
        bool guarded = false;            // a branch forward may jump over it
        int opens = 0;                   // loops that start with it
        std::vector<std::size_t> closes; // back edges that end loops here are in `back`
    };
    std::size_t index_at(std::uint32_t address) {
        const auto found = index_of_.find(address);
        if (found == index_of_.end())
            give_up("branch into the middle of an instruction");
        return found->second;
    }
    bool branch(const Ins &ins) {
        return ins.format == F::sopp && (ins.name == "s_branch" || starts(ins.name, "s_cbranch"));
    }
    std::uint32_t target_of(const Ins &ins) {
        return ins.at + 1 + static_cast<std::int16_t>(ins.w0 & 0xffff);
    }
    void find_blocks() {
        std::set<std::size_t> leaders{0};
        for (std::size_t i = 0; i < code_list_.size(); ++i)
            if (branch(code_list_[i])) {
                leaders.insert(index_at(target_of(code_list_[i])));
                leaders.insert(i + 1);
            } else if (code_list_[i].name == "s_endpgm") {
                leaders.insert(i + 1);
            }
        leaders.erase(code_list_.size());
        for (auto it = leaders.begin(); it != leaders.end(); ++it) {
            const auto next = std::next(it);
            blocks_.push_back({*it, next == leaders.end() ? code_list_.size() : *next});
        }
        const auto block_of = [&](std::size_t instruction) {
            for (std::size_t b = 0; b < blocks_.size(); ++b)
                if (instruction >= blocks_[b].first && instruction < blocks_[b].end)
                    return b;
            return blocks_.size();
        };
        target_block_.assign(blocks_.size(), ~std::size_t{0});
        for (std::size_t b = 0; b < blocks_.size(); ++b) {
            const auto &last = code_list_[blocks_[b].end - 1];
            if (last.name == "s_endpgm") { // the end, from anywhere: over everything after
                for (std::size_t over = b + 1; over < blocks_.size(); ++over)
                    blocks_[over].guarded = true;
                continue;
            }
            if (!branch(last))
                continue;
            const auto to = block_of(index_at(target_of(last)));
            target_block_[b] = to;
            if (to > b) {
                for (std::size_t over = b + 1; over < to && over < blocks_.size(); ++over)
                    blocks_[over].guarded = true;
            } else {
                ++blocks_[to].opens;
                loop_end_[to] = std::max(loop_end_[to], b);
            }
        }
        // one loop for each header, up to its last back edge
        for (auto &[header, end] : loop_end_)
            blocks_[header].opens = 1;
        // a loop that another starts in ends no sooner than that one
        for (bool changed = true; changed;) {
            changed = false;
            for (auto &[header, end] : loop_end_)
                for (const auto &[inner, inner_end] : loop_end_)
                    if (inner > header && inner <= end && inner_end > end)
                        end = inner_end, changed = true;
        }
        // the loops a branch back leaves on its way to the loop it is for
        for (std::size_t b = 0; b < blocks_.size(); ++b)
            if (const auto to = target_block_[b]; to <= b)
                for (const auto &[header, end] : loop_end_)
                    if (header > to && header <= b && end >= b)
                        left_.insert(header);
    }

    std::string s(std::uint32_t reg) {
        used_s_.insert(reg);
        return "s" + std::to_string(reg);
    }
    std::string v(std::uint32_t reg) {
        used_v_.insert(reg & 0xff);
        return "v" + std::to_string(reg & 0xff);
    }
    static std::string hex(std::uint32_t value) {
        char text[16];
        std::snprintf(text, sizeof text, "0x%08xu", value);
        return text;
    }
    static std::string f(const std::string &bits) { return "asfloat(" + bits + ")"; }
    static std::string u(const std::string &value) { return "asuint(" + value + ")"; }
    // the constant an operand code stands for, if it is one
    static std::optional<std::uint32_t> constant_of(std::uint32_t source, std::uint32_t literal) {
        if (source >= 128 && source <= 192)
            return source - 128;
        if (source >= 193 && source <= 208)
            return static_cast<std::uint32_t>(-static_cast<std::int32_t>(source - 192));
        static constexpr float floats[] = {0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -2.0f, 4.0f, -4.0f};
        if (source >= 240 && source <= 247)
            return std::bit_cast<std::uint32_t>(floats[source - 240]);
        if (source == 248)
            return 0x3e22f983; // 1 / (2 pi)
        if (source == 255)
            return literal;
        return std::nullopt;
    }
    // an operand: a scalar register, a constant, or (from 256) a vector register
    std::string operand(const Ins &ins, std::uint32_t source) {
        if (source >= 256)
            return v(source - 256);
        if (const auto value = constant_of(source, ins.literal))
            return hex(*value);
        if (source == 253)
            return "(scc ? 1u : 0u)";
        if (source == 251)
            return "((" + s(106) + " | " + s(107) + ") == 0u ? 1u : 0u)";
        if (source == 252)
            return "((" + s(126) + " | " + s(127) + ") == 0u ? 1u : 0u)";
        if (source == 125)
            return "0u";
        if (source < 128) {
            const auto &known = known_[source];
            if (!stands_in(known) || known.maybe)
                return s(source);
            // an address of the console's has no meaning here, and only goes into the
            // descriptor a shader makes of it
            if (known.kind == Known::root || known.kind == Known::memory ||
                known.kind == Known::streams || known.kind == Known::stream)
                return "0u";
            // what a cached program computes from a descriptor's words goes back into
            // them (see track), and is nothing here
            if (known.kind == Known::descriptor && shader_.heaps && known.part != 0)
                return "0u";
            if (known.kind == Known::descriptor)
                return descriptor_word(source);
            give_up("a descriptor table used as a value (" + std::string(ins.name) + ")");
        }
        give_up("operand " + std::to_string(source));
    }
    // the upper half of a 64-bit operand
    std::string operand_high(const Ins &ins, std::uint32_t source) {
        if (const auto value = constant_of(source, ins.literal))
            return source >= 193 && source <= 208 ? "0xffffffffu" : "0u";
        if (source < 127)
            return operand(ins, source + 1);
        give_up("64-bit operand " + std::to_string(source));
    }
    void forget(std::uint32_t reg) {
        if (reg < 128)
            known_[reg] = {};
    }
    // A word of a descriptor as a value. Shaders read a texture's size out of its
    // descriptor instead of asking for it; the words with the size are made from what the
    // host says of the resource.
    std::string descriptor_word(std::uint32_t reg) {
        const auto known = known_[reg];
        if (known.several)
            throw Choose{known.choice - 1};
        const auto type = facts_.types.find(known.origin);
        if (type == facts_.types.end()) {
            wanted_.insert(known.origin);
            return s(reg);
        }
        const auto base = reg - known.part;
        if (reg < known.part || known_[base].kind != Known::descriptor ||
            known_[base].origin != known.origin || known_[base].part != 0)
            give_up("a descriptor used as a value");
        const auto name = object(resource_of(base), type->second);
        const auto id = "x" + std::to_string(temporaries_++);
        std::string_view kind = type->second;
        const bool writable = starts(kind, "RW");
        if (writable)
            kind.remove_prefix(2);
        kind = kind.substr(0, kind.find('<'));
        if (kind == "StructuredBuffer" && known.part == 2) {
            line("uint " + id + ", " + id + "s; " + name + ".GetDimensions(" + id + ", " + id +
                 "s);");
            return id;
        }
        if (kind == "Buffer" && known.part == 2) { // how many elements it has
            line("uint " + id + "; " + name + ".GetDimensions(" + id + ");");
            return id;
        }
        static constexpr std::pair<std::string_view, const char *> sizes[] = {
            {"Texture1D", "w"},      {"Texture2D", "w, h"},          {"Texture3D", "w, h, d"},
            {"TextureCube", "w, h"}, {"Texture1DArray", "w, d"},     {"Texture2DArray", "w, h, d"},
            {"Texture2DMS", "w, h"}, {"Texture2DMSArray", "w, h, d"}};
        const char *size = nullptr;
        for (const auto &[texture, has] : sizes)
            if (kind == texture)
                size = has;
        if (!size || known.part < 1 || known.part > 4)
            give_up("a descriptor used as a value");
        std::string arguments;
        for (; *size; ++size)
            arguments += *size == ',' || *size == ' ' ? std::string(1, *size) : id + *size;
        if (kind.find("MS") != std::string_view::npos)
            arguments += ", " + id + "l";
        else if (!writable)
            arguments = "0u, " + arguments + ", " + id + "l";
        line("uint " + id + "w = 1u, " + id + "h = 1u, " + id + "d = 1u, " + id + "l = 1u; " +
             name + ".GetDimensions(" + arguments + ");");
        // the width less one over the second and third words, the height after it; the last
        // level in the fourth; the depth or the last layer in the fifth
        if (known.part == 1)
            return "((" + id + "w - 1u) << 30)";
        if (known.part == 2)
            return "(((" + id + "w - 1u) >> 2) | ((" + id + "h - 1u) << 14))";
        if (known.part == 3)
            return "((" + id + "l - 1u) << 16)";
        return "(" + id + "d - 1u)";
    }

    // The blocks are not laid out in the order they run in, so what a register holds where
    // a block starts is what its predecessors leave, joined.
    void analyse() {
        entry_.assign(blocks_.size(), State{});
        std::vector<bool> reached(blocks_.size(), false);
        entry_[0] = known_;
        reached[0] = true;
        std::vector<std::size_t> work{0};
        bool met = false;
        for (int budget = 0; !work.empty(); ++budget) {
            if (budget > 200000)
                give_up("descriptors that do not settle");
            const auto block = work.back();
            work.pop_back();
            auto state = entry_[block];
            for (auto i = blocks_[block].first; i < blocks_[block].end; ++i)
                track(code_list_[i], state);
            const auto &last = code_list_[blocks_[block].end - 1];
            std::size_t next[2], count = 0;
            if (branch(last))
                next[count++] = target_block_[block];
            if (last.name != "s_endpgm" && last.name != "s_branch" && block + 1 < blocks_.size())
                next[count++] = block + 1;
            for (std::size_t n = 0; n < count; ++n) {
                const auto to = next[n];
                if (!reached[to]) {
                    entry_[to] = state;
                    reached[to] = true;
                    work.push_back(to);
                    continue;
                }
                bool changed = false;
                for (std::size_t reg = 0; reg < known_slots; ++reg) {
                    // two descriptors of one table: either, by a variable set where each
                    // is loaded
                    const auto &a = entry_[to][reg], &b = state[reg];
                    if (a.kind == Known::descriptor && b.kind == Known::descriptor &&
                        a.value == b.value && a.part == b.part && a.dynamic < 0 && b.dynamic < 0 &&
                        a.origin != b.origin && !(a.choice && a.choice == b.choice))
                        met |= facts_.meet(a.origin, b.origin);
                    const auto joined = join(entry_[to][reg], state[reg]);
                    changed |= !(joined == entry_[to][reg]);
                    entry_[to][reg] = joined;
                }
                if (changed)
                    work.push_back(to);
            }
        }
        if (met)
            throw Retry{};
    }
    int dynamic_of(const Ins &ins) {
        return dynamic_id_.try_emplace(ins.at, static_cast<int>(dynamic_id_.size())).first->second;
    }
    // the place that stands for one lane of a vector register a shader keeps a scalar in
    std::optional<std::uint32_t> spill(std::uint32_t reg, std::uint32_t lane, bool make) {
        const auto found = spills_.find({reg, lane});
        if (found != spills_.end())
            return found->second;
        if (!make || spills_.size() >= known_slots - 128)
            return std::nullopt;
        const auto slot = static_cast<std::uint32_t>(128 + spills_.size());
        spills_[{reg, lane}] = slot;
        return slot;
    }
    // whether an operand is a mask of lanes: EXEC, VCC, or what was made of them
    static bool lanes(const State &known, std::uint32_t source) {
        return source == 126 || source == 127 ||
               (source < 128 && known[source].kind == Known::lanes);
    }
    // what an instruction does to what is known of the registers it writes
    void track(const Ins &ins, State &known) {
        const auto name = ins.name;
        const auto clear = [&](std::uint32_t reg, std::uint32_t count = 1) {
            for (std::uint32_t n = 0; n < count; ++n)
                if (reg + n < 128)
                    known[reg + n] = {};
        };
        const bool wide = ends(name, "_b64") || ends(name, "_i64") || ends(name, "_u64") ||
                          name == "s_bitreplicate_b64_b32";
        const auto mask = [&](std::uint32_t reg) {
            for (std::uint32_t n = 0; n < (wave64_ ? 2u : 1u); ++n)
                if (reg + n < 128)
                    known[reg + n] = {Known::lanes};
        };
        switch (ins.format) {
        case F::sop1: {
            const auto destination = (ins.w0 >> 16) & 0x7f, source = ins.w0 & 0xff;
            if (name == "s_mov_b32" || name == "s_mov_b64") {
                const auto value = constant_of(source, ins.literal);
                for (std::uint32_t n = 0; n < (wide ? 2u : 1u); ++n)
                    if (destination + n < 128)
                        known[destination + n] = lanes(known, source + n) ? Known{Known::lanes}
                                                 : source + n < 128       ? known[source + n]
                                                 : value && !n ? Known{Known::constant, *value}
                                                               : Known{};
            } else if (const auto value = constant_of(source, ins.literal);
                       name == "s_brev_b32" && value && destination < 128) {
                // a word of a sampler made in the code may be written the short way round
                std::uint32_t reversed = 0;
                for (int bit = 0; bit < 32; ++bit)
                    reversed |= (*value >> bit & 1) << (31 - bit);
                known[destination] = {Known::constant, reversed};
            } else if (name == "s_getpc_b64") {
                known[destination] = {Known::code};
                clear(destination + 1);
            } else if (name.find("saveexec") != std::string_view::npos) {
                for (std::uint32_t n = 0; n < (wide ? 2u : 1u); ++n)
                    if (destination + n < 128)
                        known[destination + n] = {Known::lanes};
            } else if (name != "s_swappc_b64") {
                clear(destination, wide && name.find("_i32_") == std::string_view::npos ? 2 : 1);
            }
            break;
        }
        case F::sop2: {
            const auto destination = (ins.w0 >> 16) & 0x7f, first = ins.w0 & 0xff,
                       second = (ins.w0 >> 8) & 0xff;
            Known result;
            if ((name == "s_add_u32" || name == "s_add_i32") &&
                ((first < 128 && known[first].kind == Known::code) ||
                 (second < 128 && known[second].kind == Known::code)))
                result = {Known::code};
            // the upper half of a root descriptor's address with a stride put in: on its
            // way to being a buffer descriptor
            if (name == "s_or_b32" || name == "s_and_b32")
                for (const auto &[reg, other] :
                     {std::pair{first, second}, std::pair{second, first}})
                    if (reg < 128 && known[reg].kind == Known::root && known[reg].part == 1) {
                        result = known[reg];
                        if (const auto bits = constant_of(other, ins.literal);
                            bits && name == "s_or_b32")
                            result.stride = (*bits >> 16) & 0x3fff;
                    }
            // the address of the vertex streams with its upper bits cut off is still that
            if (starts(name, "s_bfe_") && first < 128 && known[first].kind == Known::streams)
                result = known[first];
            // masks of lanes combined are one
            if (wide && !starts(name, "s_lsh") && !starts(name, "s_ashr") && lanes(known, first) &&
                lanes(known, second))
                result = {Known::lanes};
            // A cached program changes the later words of a descriptor it has loaded (the
            // stride and size of a buffer it reads by bytes): it is that descriptor still,
            // and the host's view is read as the program then reads it.
            const auto before = destination < 128 ? known[destination] : Known{};
            clear(destination, wide ? 2 : 1);
            if (destination < 128)
                known[destination] = result;
            if (shader_.heaps && !wide && result.kind == Known::nothing &&
                before.kind == Known::descriptor && before.part != 0)
                known[destination] = before;
            if (result.kind == Known::lanes && destination + 1 < 128)
                known[destination + 1] = result;
            break;
        }
        case F::sopk: {
            const auto destination = (ins.w0 >> 16) & 0x7f;
            if (name == "s_movk_i32")
                known[destination] = {
                    Known::constant,
                    static_cast<std::uint32_t>(static_cast<std::int16_t>(ins.w0 & 0xffff))};
            else if (!starts(name, "s_cmpk_") && !starts(name, "s_setreg") && name != "s_version")
                clear(destination);
            break;
        }
        case F::smem: {
            const auto data = (ins.w0 >> 6) & 0x7f, base = (ins.w0 & 0x3f) * 2;
            if (!starts(name, "s_load_dword") && !starts(name, "s_buffer_load_dword"))
                break;
            const auto rest = name.substr(name.rfind("dword") + 5);
            const std::uint32_t words = rest.empty()   ? 1
                                        : rest == "x2" ? 2
                                        : rest == "x4" ? 4
                                        : rest == "x8" ? 8
                                                       : 16;
            const auto from = known[base];
            clear(data, words);
            if (starts(name, "s_load_dword") && from.kind == Known::memory) {
                for (std::uint32_t n = 0; n < words && ins.w1 >> 25 == 0x7d; ++n) {
                    const auto place = (ins.w1 & 0x1fffff) / 4 + n;
                    const auto *const placed = in_memory(place);
                    if (!placed || data + n >= 128)
                        continue;
                    if (parameters_[placed->parameter].type == 0)
                        known[data + n] = {Known::table, placed->parameter};
                    else if (parameters_[placed->parameter].type >= 2)
                        known[data + n] = {Known::root, placed->parameter, 0, place - placed->at};
                }
            }
            if (starts(name, "s_load_dword") && from.kind == Known::streams)
                for (std::uint32_t n = 0; n < words; ++n)
                    if (data + n < 128)
                        known[data + n] = {Known::stream, (ins.w1 & 0x1fffff) / 16, 0, n};
            if (starts(name, "s_load_dword") && from.kind == Known::table) {
                Known loaded{Known::descriptor, from.value, ins.w1 & 0x1fffff};
                loaded.origin = ins.at;
                if (ins.w1 >> 25 != 0x7d) {
                    loaded.dynamic = dynamic_of(ins);
                } else if (facts_.met.count(ins.at)) {
                    loaded.choice = facts_.first_of(ins.at) + 1;
                }
                for (std::uint32_t n = 0; n < words; ++n)
                    if (data + n < 128) {
                        known[data + n] = loaded;
                        known[data + n].part = n;
                    }
            }
            break;
        }
        case F::vop1:
            if (name == "v_readfirstlane_b32")
                clear((ins.w0 >> 17) & 0x7f);
            break;
        case F::vopc:
            if (ins.form == Ins::Form::sub_dword && ins.modifier >> 15 & 1)
                mask((ins.modifier >> 8) & 0x7f);
            else
                mask(106);
            break;
        case F::vop2:
            if (name.find("_co_") != std::string_view::npos)
                clear(106, 2);
            break;
        case F::vop3: {
            const auto destination = ins.w0 & 0xff;
            const std::uint32_t codes[2] = {ins.w1 & 0x1ff, (ins.w1 >> 9) & 0x1ff};
            if (ins.op < 0x100) {
                mask(destination & 0x7f);
            } else if (name.find("_co_") != std::string_view::npos) {
                clear((ins.w0 >> 8) & 0x7f, wave64_ ? 2 : 1);
            } else if (name == "v_writelane_b32") {
                if (const auto lane = constant_of(codes[1], ins.literal))
                    if (const auto slot = spill(destination, *lane, true))
                        known[*slot] = codes[0] < 128 ? known[codes[0]] : Known{};
            } else if (name == "v_readlane_b32") {
                clear(destination & 0x7f);
                if (const auto lane = constant_of(codes[1], ins.literal); lane && codes[0] >= 256)
                    if (const auto slot = spill(codes[0] - 256, *lane, false))
                        known[destination & 0x7f] = known[*slot];
            }
            break;
        }
        default:
            break;
        }
    }
    std::string active() { return "(" + s(126) + " != 0u)"; }
    void line(const std::string &text) { body_ += indent_ + text + "\n"; }
    // a word as it is now, for an instruction that may write the registers it is read from
    // before it has read all it needs
    std::string keep(const std::string &value) {
        const auto id = "x" + std::to_string(temporaries_++);
        line("uint " + id + " = " + value + ";");
        return id;
    }
    // a write to a vector register, which only the lanes of EXEC do
    void set_v(std::uint32_t reg, const std::string &whole) {
        const auto value = placed_ ? place(reg, whole) : whole;
        if (exec_changes_)
            line("if " + active() + " " + v(reg) + " = " + value + ";");
        else
            line(v(reg) + " = " + value + ";");
    }
    void set_s(std::uint32_t reg, const std::string &value) {
        if (reg == 125)
            return;
        forget(reg);
        line(s(reg) + " = " + value + ";");
    }
    void set_mask(std::uint32_t reg, const std::string &condition) {
        const auto value = "((" + condition + ")" + (exec_changes_ ? " && " + active() : "") +
                           " ? 0xffffffffu : 0u)";
        set_s(reg, value);
        if (wave64_ && reg < 127) {
            forget(reg + 1);
            line(s(reg + 1) + " = " + s(reg) + ";");
        }
    }

    void prepare_registers() {
        used_s_.insert({106, 107, 126, 127});
        for (const auto &placed : in_registers_)
            for (std::uint32_t n = 0; n < placed.words; ++n)
                prologue_ +=
                    "    " + s(placed.at + n) + " = " + root_constant(placed.parameter, n) + ";\n";
        if (stage_ == Stage::pixel) {
            // the position's components that are read follow the barycentrics
            std::uint32_t reg = 2;
            for (std::size_t i = 0; i < shader_.inputs.size(); ++i) {
                const auto &element = shader_.inputs[i];
                if (_stricmp(element.name.c_str(), "SV_Position"))
                    continue;
                for (int c = 0; c < 4; ++c)
                    if (element.read >> c & 1)
                        prologue_ += "    " + v(reg++) + " = asuint(input.f" + std::to_string(i) +
                                     "." + "xyzw"[c] + ");\n";
            }
        } else if (stage_ == Stage::compute) {
            // after the user registers, the thread group's id as enabled; the thread's id
            // within its group in the first vector registers
            std::uint32_t reg = (header_[0] >> 1) & 0x1f;
            for (int c = 0; c < 3; ++c)
                if (header_[0] >> (7 + c) & 1)
                    prologue_ += "    " + s(reg++) + " = group." + "xyz"[c] + ";\n";
            for (int c = 0; c < 3; ++c)
                prologue_ += "    " + v(c) + " = thread." + "xyz"[c] + ";\n";
        } else if (stage_ == Stage::mesh) {
            // the wave's place in the group in bits 24 to 27 of s3, and the group's number in
            // v5, as many times the most of its lanes, vertices and primitives as it is
            prologue_ +=
                "    " + s(3) + " = (xv_flat / " + std::to_string(mesh_wave_) + "u) << 24;\n";
            const auto stride = std::max({mesh_threads_, mesh_vertices_, mesh_primitives_});
            prologue_ += "    " + v(5) + " = xv_group.x * " + std::to_string(stride) + "u;\n";
        }
    }

    void emit_block(std::size_t index) {
        auto &block = blocks_[index];
        known_ = entry_[index];
        if (block.opens) {
            line("for (uint loop" + std::to_string(index) + " = 0u; loop" + std::to_string(index) +
                 " < 4096u; ++loop" + std::to_string(index) + ") {");
            indent_ += "    ";
            open_loops_.push_back(index);
        }
        if (block.guarded) {
            line("if (next <= " + std::to_string(index) + "u) {");
            indent_ += "    ";
        }
        for (std::size_t i = block.first; i < block.end; ++i) {
            const auto &ins = code_list_[i];
            if (i + 1 == block.end && (branch(ins) || ins.name == "s_endpgm"))
                break;
            // what the instruction makes of what was known before it: writing a register
            // forgets it, and the instruction may be giving it what it reads from itself
            auto tracked = known_;
            track(ins, tracked);
            try {
                emit_chosen(ins);
            } catch (Unsupported &unsupported) {
                if (unsupported.at == ~0u)
                    unsupported.at = ins.at;
                throw;
            }
            known_ = tracked;
        }
        const auto &last = code_list_[block.end - 1];
        const auto to = target_block_[index];
        std::string leave; // what ends the loop that closes here, if it is not taken again
        if (last.name == "s_endpgm") {
            line("next = " + std::to_string(blocks_.size()) + "u;");
        } else if (branch(last)) {
            const auto condition = branch_condition(last);
            if (to > index)
                line((condition.empty() ? "" : "if (" + condition + ") ") +
                     "next = " + std::to_string(to) + "u;");
            else if (!open_loops_.empty() && loop_end_[to] == index && open_loops_.back() == to)
                leave = condition.empty() ? "" : "if (!(" + condition + ")) break;";
            else // a back edge that is not its loop's last: once more round, of that loop
                line((condition.empty() ? "" : "if (" + condition + ") ") +
                     "{ next = " + std::to_string(to) + "u; " +
                     (open_loops_.back() == to ? "continue;" : "back = true; break;") + " }");
        }
        if (block.guarded) {
            indent_.resize(indent_.size() - 4);
            line("}");
        }
        while (!open_loops_.empty() && loop_end_[open_loops_.back()] == index) {
            const auto header = open_loops_.back();
            // out of the loop if a branch went past its end, or its back edge is not taken
            line("if (next > " + std::to_string(index) + "u) break;");
            if (target_block_[index] == header &&
                code_list_[blocks_[index].end - 1].name != "s_endpgm") {
                if (!leave.empty())
                    line(leave);
                // round again: what a branch forward skipped is not skipped the next time
                line("next = " + std::to_string(header) + "u;");
            } else {
                line("break;");
            }
            indent_.resize(indent_.size() - 4);
            line("}");
            open_loops_.pop_back();
            if (left_.count(header))
                line("if (back) { if (next >= " + std::to_string(open_loops_.back()) +
                     "u) { back = false; continue; } break; }");
        }
    }
    // An instruction, once for each descriptor it may have been given where it uses one
    // that is one of several.
    void emit_chosen(const Ins &ins) {
        const auto size = body_.size();
        const auto known = known_;
        const auto shared = shared_;
        try {
            emit(ins);
            return;
        } catch (const Choose &choose) {
            body_.resize(size);
            shared_ = shared;
            std::optional<Unsupported> mismatch;
            bool any = false;
            // first the one loaded last before the instruction, as the code is laid out:
            // what it is used as here is what it is
            std::vector<std::uint32_t> origins;
            for (const auto &[origin, first] : facts_.met)
                if (facts_.first_of(origin) == choose.first)
                    origins.push_back(origin);
            const auto after = std::partition_point(origins.begin(), origins.end(),
                                                    [&](auto origin) { return origin < ins.at; });
            const bool certain = after != origins.begin();
            if (certain)
                std::rotate(origins.begin(), after - 1, after);
            for (const auto origin : origins) {
                if (!(certain && origin == origins[0]) && !facts_.learned &&
                    !facts_.types.count(origin)) {
                    unlearned_ = true;
                    continue;
                }
                const auto before = body_.size();
                known_ = known;
                for (auto &one : known_)
                    if (one.kind == Known::descriptor && one.several &&
                        one.choice == choose.first + 1) {
                        one.origin = origin;
                        one.offset = code_list_[index_at(origin)].w1 & 0x1fffff;
                        one.several = false;
                    }
                line("if (c" + std::to_string(choose.first) + " == " + std::to_string(origin) +
                     "u) {");
                indent_ += "    ";
                try {
                    emit_chosen(ins);
                    any = true;
                } catch (const Unsupported &unsupported) {
                    // A descriptor of another kind than the instruction takes was not
                    // loaded for it: it is what the register held on a way that skips
                    // both the load and, with no lane active, the instruction.
                    if (!unsupported.mismatch)
                        throw;
                    mismatch = unsupported;
                    indent_.resize(indent_.size() - 4);
                    body_.resize(before);
                    shared_ = shared;
                    continue;
                }
                indent_.resize(indent_.size() - 4);
                line("}");
            }
            if (!any && mismatch)
                throw *mismatch;
            known_ = known;
        }
    }
    std::string branch_condition(const Ins &ins) {
        if (ins.name == "s_branch")
            return {};
        if (ins.name == "s_cbranch_scc0")
            return "!scc";
        if (ins.name == "s_cbranch_scc1")
            return "scc";
        if (ins.name == "s_cbranch_vccz")
            return "(" + s(106) + " | " + s(107) + ") == 0u";
        if (ins.name == "s_cbranch_vccnz")
            return "(" + s(106) + " | " + s(107) + ") != 0u";
        if (ins.name == "s_cbranch_execz")
            return "(" + s(126) + " | " + s(127) + ") == 0u";
        if (ins.name == "s_cbranch_execnz")
            return "(" + s(126) + " | " + s(127) + ") != 0u";
        // COND_DBG_SYS and COND_DBG_USER of STATUS are clear outside a debugger
        if (ins.name == "s_cbranch_cdbgsys" || ins.name == "s_cbranch_cdbguser" ||
            ins.name == "s_cbranch_cdbgsys_or_user" || ins.name == "s_cbranch_cdbgsys_and_user")
            return "false";
        give_up(std::string(ins.name));
    }

    void emit(const Ins &ins) {
        switch (ins.format) {
        case F::sopp:
            emit_sopp(ins);
            break;
        case F::sop1:
            emit_sop1(ins);
            break;
        case F::sop2:
            emit_sop2(ins);
            break;
        case F::sopk:
            emit_sopk(ins);
            break;
        case F::sopc:
            emit_sopc(ins);
            break;
        case F::smem:
            emit_smem(ins);
            break;
        case F::vop1:
        case F::vop2:
        case F::vopc:
            emit_short(ins);
            break;
        case F::ds:
            emit_shared(ins);
            break;
        case F::vop3:
            emit_vop3(ins);
            break;
        case F::vop3p:
            emit_vop3p(ins);
            break;
        case F::vintrp:
            emit_interpolation(ins);
            break;
        case F::mimg:
            emit_image(ins);
            break;
        case F::mubuf:
        case F::mtbuf: // the same fields, and a format of its own
            emit_buffer(ins);
            break;
        case F::exp:
            emit_export(ins);
            break;
        default:
            give_up(std::string(ins.name));
        }
    }

    void emit_sopp(const Ins &ins) {
        static constexpr std::string_view nothing[] = {
            "s_nop",          "s_waitcnt",      "s_inst_prefetch",
            "s_code_end",     "s_barrier",      "s_sendmsg",
            "s_clause",       "s_wait_idle",    "s_waitcnt_depctr",
            "s_denorm_mode",  "s_ttracedata",   "s_ttracedata_imm",
            "s_incperflevel", "s_decperflevel", "s_icache_inv",
            "s_wakeup",       "s_sleep",        "s_setprio"};
        if (ins.name == "s_barrier" && uses_shared_) {
            synchronize();
            return;
        }
        if (std::find(std::begin(nothing), std::end(nothing), ins.name) != std::end(nothing))
            return;
        // FP_ROUND of MODE: 0 in both fields is round to nearest even, the host's only mode
        if (ins.name == "s_round_mode" && !(ins.w0 & 0xf))
            return;
        give_up(std::string(ins.name));
    }
    void emit_sop1(const Ins &ins) {
        const auto destination = (ins.w0 >> 16) & 0x7f, source = ins.w0 & 0xff;
        const auto name = ins.name;
        if (name == "s_swappc_b64") { // the call to the fetch shader
            fetch_inputs();
            return;
        }
        // what stands for something of the console's is moved by what is known of it alone
        const auto only_known = [&](std::uint32_t reg) {
            return reg < 128 && stands_in(known_[reg]) && !known_[reg].maybe;
        };
        if (name == "s_mov_b32") {
            // lanes named one by one: this one is the first
            if (const auto lanes = constant_of(source, ins.literal);
                destination == 126 && lanes && !(*lanes & 1))
                set_s(destination, "0u");
            else if (!only_known(source))
                set_s(destination, operand(ins, source));
            return;
        }
        if (name == "s_mov_b64" && source < 127 && (only_known(source) || only_known(source + 1))) {
            for (std::uint32_t half = 0; half < 2; ++half)
                if (!only_known(source + half))
                    set_s(destination + half, operand(ins, source + half));
            return;
        }
        if (name == "s_getpc_b64") { // where the next instruction is, from the first
            set_s(destination, hex(4 * (ins.at + 1)));
            set_s(destination + 1, "0u");
            return;
        }
        if (name == "s_bitset0_b32" || name == "s_bitset1_b32") {
            const auto bit = "(1u << (" + operand(ins, source) + " & 31u))";
            const auto old = s(destination);
            set_s(destination, name == "s_bitset1_b32" ? old + " | " + bit : old + " & ~" + bit);
            return;
        }
        if (name == "s_bitset0_b64" || name == "s_bitset1_b64") {
            const bool set = name == "s_bitset1_b64";
            line("{ uint bit = " + operand(ins, source) +
                 " & 63u, lo = bit < 32u ? 1u << bit : 0u, hi = bit < 32u ? 0u : 1u << (bit - "
                 "32u);");
            const auto low = s(destination), high = s(destination + 1);
            set_s(destination, set ? low + " | lo" : low + " & ~lo");
            set_s(destination + 1, set ? high + " | hi" : high + " & ~hi");
            line("}");
            return;
        }
        if (name == "s_mov_b64" || name == "s_wqm_b64" || name == "s_not_b64") {
            const auto low = operand(ins, source), high = operand_high(ins, source);
            const bool inverted = name == "s_not_b64";
            line("{ uint lo = " + std::string(inverted ? "~" : "") + low +
                 ", hi = " + (inverted ? "~" : "") + high + ";");
            set_s(destination, "lo");
            set_s(destination + 1, "hi");
            if (name != "s_mov_b64")
                line("scc = (lo | hi) != 0u;");
            line("}");
            return;
        }
        if (ends(name, "saveexec_b64") || ends(name, "saveexec_b32")) {
            // the destination keeps EXEC, and EXEC becomes it combined with the source
            const bool wide = ends(name, "_b64");
            const auto combine = [&](const std::string &a, const std::string &exec) -> std::string {
                if (starts(name, "s_and_saveexec"))
                    return a + " & " + exec;
                if (starts(name, "s_or_saveexec"))
                    return a + " | " + exec;
                if (starts(name, "s_xor_saveexec"))
                    return a + " ^ " + exec;
                if (starts(name, "s_andn2_saveexec"))
                    return a + " & ~" + exec;
                if (starts(name, "s_andn1_saveexec"))
                    return "~" + a + " & " + exec;
                if (starts(name, "s_orn2_saveexec"))
                    return a + " | ~" + exec;
                if (starts(name, "s_nand_saveexec"))
                    return "~(" + a + " & " + exec + ")";
                if (starts(name, "s_nor_saveexec"))
                    return "~(" + a + " | " + exec + ")";
                if (starts(name, "s_xnor_saveexec"))
                    return "~(" + a + " ^ " + exec + ")";
                if (starts(name, "s_orn1_saveexec"))
                    return "~" + a + " | " + exec;
                give_up(std::string(name));
            };
            line("{ uint lo = " + combine(operand(ins, source), s(126)) + ", hi = " +
                 (wide ? combine(operand_high(ins, source), s(127)) : std::string("0u")) + ";");
            set_s(destination, s(126));
            if (wide)
                set_s(destination + 1, s(127));
            line(s(126) + " = lo;" + (wide ? " " + s(127) + " = hi;" : "") +
                 " scc = (lo | hi) != 0u; }");
            return;
        }
        if (starts(name, "s_andn1_wrexec") || starts(name, "s_andn2_wrexec")) {
            // EXEC combined with the source, and the destination takes what EXEC becomes
            const bool wide = ends(name, "_b64"), first = starts(name, "s_andn1");
            const auto combine = [&](const std::string &a, const std::string &exec) {
                return first ? "~" + a + " & " + exec : a + " & ~" + exec;
            };
            line("{ uint lo = " + combine(operand(ins, source), s(126)) + ", hi = " +
                 (wide ? combine(operand_high(ins, source), s(127)) : std::string("0u")) + ";");
            line(s(126) + " = lo;" + (wide ? " " + s(127) + " = hi;" : ""));
            set_s(destination, "lo");
            if (wide)
                set_s(destination + 1, "hi");
            line("scc = (lo | hi) != 0u; }");
            return;
        }
        if (name == "s_cmov_b64") {
            const auto low = operand(ins, source), high = operand_high(ins, source);
            line("if (scc) { " + s(destination) + " = " + low + "; " + s(destination + 1) + " = " +
                 high + "; }");
            forget(destination);
            forget(destination + 1);
            return;
        }
        if (name == "s_brev_b64" || name == "s_bitreplicate_b64_b32" || name == "s_quadmask_b64") {
            const auto low = operand(ins, source);
            const auto high =
                name == "s_bitreplicate_b64_b32" ? std::string("0u") : operand_high(ins, source);
            if (name == "s_brev_b64")
                line("{ uint lo = reversebits(" + high + "), hi = reversebits(" + low + ");");
            else if (name == "s_quadmask_b64")
                line("{ uint lo = quad_mask(" + low + ") | (quad_mask(" + high +
                     ") << 8), hi = 0u; scc = lo != 0u;");
            else
                line("{ uint lo = bit_pairs(" + low + " & 0xffffu), hi = bit_pairs(" + low +
                     " >> 16);");
            set_s(destination, "lo");
            set_s(destination + 1, "hi");
            line("}");
            return;
        }
        std::string value;
        const auto a = operand(ins, source);
        // the first bit that differs from the sign, counted from the top
        const auto sign_bit = [](const std::string &x) {
            return "(((int)" + x + " < 0 ? ~" + x + " : " + x +
                   ") == 0u ? 0xffffffffu : 31u - "
                   "firstbithigh((int)" +
                   x + " < 0 ? ~" + x + " : " + x + "))";
        };
        if (name == "s_bcnt0_i32_b32")
            value = "countbits(~" + a + ")";
        else if (name == "s_bcnt0_i32_b64")
            value = "(countbits(~" + a + ") + countbits(~" + operand_high(ins, source) + "))";
        else if (name == "s_ff0_i32_b32")
            value = "firstbitlow(~" + a + ")";
        else if (name == "s_ff0_i32_b64" || name == "s_ff1_i32_b64") {
            const std::string flip = name == "s_ff0_i32_b64" ? "~" : "";
            const auto low = "(" + flip + a + ")",
                       high = "(" + flip + operand_high(ins, source) + ")";
            value = "(" + low + " != 0u ? firstbitlow(" + low + ") : " + high +
                    " != 0u ? 32u + firstbitlow(" + high + ") : 0xffffffffu)";
        } else if (name == "s_flbit_i32_b64") {
            const auto high = operand_high(ins, source);
            value = "(" + high + " != 0u ? 31u - firstbithigh(" + high + ") : " + a +
                    " != 0u ? 63u - firstbithigh(" + a + ") : 0xffffffffu)";
        } else if (name == "s_flbit_i32")
            value = sign_bit(a);
        else if (name == "s_flbit_i32_i64") {
            const auto high = operand_high(ins, source);
            const auto low = "((int)" + high + " < 0 ? ~" + a + " : " + a + ")";
            value = "(" + sign_bit(high) + " != 0xffffffffu ? " + sign_bit(high) + " : " + low +
                    " != 0u ? 63u - firstbithigh(" + low + ") : 0xffffffffu)";
        } else if (name == "s_wqm_b32")
            value = a; // a lane's mask is all of it or none: its quad's is the same
        else if (name == "s_quadmask_b32")
            value = "quad_mask(" + a + ")";
        if (!value.empty()) {
            set_s(destination, value);
            if (starts(name, "s_bcnt0") || name == "s_wqm_b32" || name == "s_quadmask_b32")
                line("scc = " + s(destination) + " != 0u;");
            return;
        }
        if (name == "s_not_b32")
            value = "~" + a;
        else if (name == "s_brev_b32")
            value = "reversebits(" + a + ")";
        else if (name == "s_bcnt1_i32_b32")
            value = "countbits(" + a + ")";
        else if (name == "s_bcnt1_i32_b64" && lanes(known_, source))
            value = "(" + a + " != 0u ? 1u : 0u)"; // of a wave of this one lane
        else if (name == "s_bcnt1_i32_b64")
            value = "(countbits(" + a + ") + countbits(" + operand_high(ins, source) + "))";
        else if (name == "s_ff1_i32_b32")
            value = "firstbitlow(" + a + ")";
        else if (name == "s_flbit_i32_b32")
            value = "(" + a + " == 0u ? 0xffffffffu : 31u - firstbithigh(" + a + "))";
        else if (name == "s_sext_i32_i8")
            value = "(uint)(((int)(" + a + " << 24)) >> 24)";
        else if (name == "s_sext_i32_i16")
            value = "(uint)(((int)(" + a + " << 16)) >> 16)";
        else if (name == "s_abs_i32")
            value = "(uint)abs((int)" + a + ")";
        else if (name == "s_cmov_b32") {
            line("if (scc) " + s(destination) + " = " + a + ";");
            forget(destination);
            return;
        } else
            give_up(std::string(name));
        set_s(destination, value);
        if (name == "s_not_b32" || starts(name, "s_bcnt1") || name == "s_abs_i32")
            line("scc = " + s(destination) + " != 0u;");
    }
    void emit_sop2(const Ins &ins) {
        const auto destination = (ins.w0 >> 16) & 0x7f, first = ins.w0 & 0xff,
                   second = (ins.w0 >> 8) & 0xff;
        const auto name = ins.name;
        if (ends(name, "_b64") && name != "s_lshl_b64" && name != "s_lshr_b64" &&
            name != "s_ashr_i64" && name != "s_bfm_b64" && name != "s_cselect_b64") {
            const auto combine = [&](const std::string &a, const std::string &b) -> std::string {
                if (name == "s_and_b64")
                    return a + " & " + b;
                if (name == "s_or_b64")
                    return a + " | " + b;
                if (name == "s_xor_b64")
                    return a + " ^ " + b;
                if (name == "s_andn2_b64")
                    return a + " & ~" + b;
                if (name == "s_orn2_b64")
                    return a + " | ~" + b;
                if (name == "s_nand_b64")
                    return "~(" + a + " & " + b + ")";
                if (name == "s_nor_b64")
                    return "~(" + a + " | " + b + ")";
                if (name == "s_xnor_b64")
                    return "~(" + a + " ^ " + b + ")";
                give_up(std::string(name));
            };
            line("{ uint lo = " + combine(operand(ins, first), operand(ins, second)) +
                 ", hi = " + combine(operand_high(ins, first), operand_high(ins, second)) + ";");
            set_s(destination, "lo");
            set_s(destination + 1, "hi");
            line("scc = (lo | hi) != 0u; }");
            return;
        }
        if (name == "s_lshl_b64" || name == "s_lshr_b64" || name == "s_ashr_i64" ||
            name == "s_bfm_b64") {
            const auto count = operand(ins, second);
            const char *const how = name == "s_lshl_b64"   ? "shift_left"
                                    : name == "s_lshr_b64" ? "shift_right"
                                                           : "shift_signed";
            if (name == "s_bfm_b64")
                line("{ uint lo, hi; field_mask(" + operand(ins, first) + ", " + count +
                     ", lo, hi);");
            else
                line("{ uint lo, hi; " + std::string(how) + "(" + operand(ins, first) + ", " +
                     operand_high(ins, first) + ", " + count + ", lo, hi);");
            set_s(destination, "lo");
            set_s(destination + 1, "hi");
            line(name == "s_bfm_b64" ? "}" : "scc = (lo | hi) != 0u; }");
            return;
        }
        if (name == "s_bfe_u64" || name == "s_bfe_i64") {
            // a field of `width` bits from bit `first`, which is cut where the source ends
            const auto field = operand(ins, second);
            line("{ uint first = " + field + " & 63u, width = min((" + field +
                 " >> 16) & 127u, 64u - first), lo = 0u, hi = 0u;");
            line("if (width != 0u) { shift_left(" + operand(ins, first) + ", " +
                 operand_high(ins, first) + ", 64u - width - first, lo, hi); " +
                 (name == "s_bfe_u64" ? "shift_right" : "shift_signed") +
                 "(lo, hi, 64u - width, lo, hi); }");
            set_s(destination, "lo");
            set_s(destination + 1, "hi");
            line("scc = (lo | hi) != 0u; }");
            return;
        }
        if (name == "s_cselect_b64") {
            line("{ uint lo = scc ? " + operand(ins, first) + " : " + operand(ins, second) +
                 ", hi = scc ? " + operand_high(ins, first) + " : " + operand_high(ins, second) +
                 ";");
            set_s(destination, "lo");
            set_s(destination + 1, "hi");
            line("}");
            return;
        }
        const auto a = operand(ins, first), b = operand(ins, second);
        std::string value, condition = "X != 0u";
        if (name == "s_add_u32")
            value = a + " + " + b, condition = "X < " + a;
        else if (name == "s_sub_u32")
            value = a + " - " + b, condition = b + " > " + a;
        else if (name == "s_add_i32")
            value = a + " + " + b, condition = "false";
        else if (name == "s_sub_i32")
            value = a + " - " + b, condition = "false";
        else if (name == "s_addc_u32")
            value = a + " + " + b + " + (scc ? 1u : 0u)",
            condition = "X < " + a + " || (scc && X == " + a + ")";
        else if (name == "s_subb_u32")
            value = a + " - " + b + " - (scc ? 1u : 0u)",
            condition = b + " > " + a + " || (scc && " + b + " == " + a + ")";
        else if (name == "s_absdiff_i32")
            value = "(uint)abs((int)(" + a + " - " + b + "))";
        else if (name == "s_pack_lh_b32_b16")
            value = "(" + a + " & 0xffffu) | (" + b + " & 0xffff0000u)", condition.clear();
        else if (name == "s_and_b32")
            value = a + " & " + b;
        else if (name == "s_or_b32")
            value = a + " | " + b;
        else if (name == "s_xor_b32")
            value = a + " ^ " + b;
        else if (name == "s_andn2_b32")
            value = a + " & ~" + b;
        else if (name == "s_orn2_b32")
            value = a + " | ~" + b;
        else if (name == "s_nand_b32")
            value = "~(" + a + " & " + b + ")";
        else if (name == "s_nor_b32")
            value = "~(" + a + " | " + b + ")";
        else if (name == "s_xnor_b32")
            value = "~(" + a + " ^ " + b + ")";
        else if (name == "s_lshl_b32")
            value = a + " << (" + b + " & 31u)";
        else if (name == "s_lshr_b32")
            value = a + " >> (" + b + " & 31u)";
        else if (name == "s_ashr_i32")
            value = "(uint)((int)" + a + " >> (" + b + " & 31u))";
        else if (name == "s_mul_i32")
            value = a + " * " + b, condition.clear();
        else if (name == "s_min_i32")
            value = "(uint)min((int)" + a + ", (int)" + b + ")",
            condition = "(int)" + a + " <= (int)" + b;
        else if (name == "s_max_i32")
            value = "(uint)max((int)" + a + ", (int)" + b + ")",
            condition = "(int)" + a + " >= (int)" + b;
        else if (name == "s_min_u32")
            value = "min(" + a + ", " + b + ")", condition = a + " <= " + b;
        else if (name == "s_max_u32")
            value = "max(" + a + ", " + b + ")", condition = a + " >= " + b;
        else if (name == "s_cselect_b32")
            value = "(scc ? " + a + " : " + b + ")", condition.clear();
        else if (name == "s_bfe_i32")
            value = "(((" + b + " >> 16) & 127u) == 0u ? 0u : (uint)(((int)(" + a +
                    " << (32u - ((" + b + " >> 16) & 127u) - (" + b + " & 31u)))) >> (32u - ((" +
                    b + " >> 16) & 127u))))";
        else if (name == "s_bfe_u32")
            value =
                "((" + a + " >> (" + b + " & 31u)) & ((1u << ((" + b + " >> 16) & 127u)) - 1u))";
        else if (name == "s_bfm_b32")
            value = "((1u << (" + a + " & 31u)) - 1u) << (" + b + " & 31u)", condition.clear();
        else if (name == "s_mul_hi_i32")
            value = "mul_hi_signed(" + a + ", " + b + ")", condition.clear();
        else if (name == "s_lshl1_add_u32")
            value = "(" + a + " << 1) + " + b, condition = "false";
        else if (name == "s_lshl2_add_u32")
            value = "(" + a + " << 2) + " + b, condition = "false";
        else if (name == "s_lshl3_add_u32")
            value = "(" + a + " << 3) + " + b, condition = "false";
        else if (name == "s_lshl4_add_u32")
            value = "(" + a + " << 4) + " + b, condition = "false";
        else if (name == "s_pack_ll_b32_b16")
            value = "(" + a + " & 0xffffu) | (" + b + " << 16)", condition.clear();
        else if (name == "s_pack_hh_b32_b16")
            value = "(" + a + " >> 16) | (" + b + " & 0xffff0000u)", condition.clear();
        else if (name == "s_mul_hi_u32")
            value = "mul_hi(" + a + ", " + b + ")", condition.clear();
        else
            give_up(std::string(name));
        line("{ uint X = " + value + ";");
        if (!condition.empty())
            line("scc = " + condition + ";");
        set_s(destination, "X");
        line("}");
    }
    void emit_sopk(const Ins &ins) {
        const auto destination = (ins.w0 >> 16) & 0x7f;
        const auto immediate =
            static_cast<std::uint32_t>(static_cast<std::int16_t>(ins.w0 & 0xffff));
        const auto name = ins.name;
        if (name == "s_movk_i32") {
            set_s(destination, hex(immediate));
        } else if (name == "s_addk_i32") {
            set_s(destination, s(destination) + " + " + hex(immediate));
        } else if (name == "s_mulk_i32") {
            set_s(destination, s(destination) + " * " + hex(immediate));
        } else if (starts(name, "s_cmpk_")) {
            const bool is_signed = name.find("_i32") != std::string_view::npos;
            const auto a = is_signed ? "(int)" + s(destination) : s(destination);
            const auto b = is_signed ? "(int)" + hex(immediate) : hex(ins.w0 & 0xffff);
            line("scc = " + a + " " + relation(name.substr(7, 2)) + " " + b + ";");
        } else if (name == "s_cmovk_i32") {
            line("if (scc) " + s(destination) + " = " + hex(immediate) + ";");
            forget(destination);
        } else if (starts(name, "s_setreg") || name == "s_getreg_b32" || name == "s_version" ||
                   starts(name, "s_waitcnt_")) {
            // the GPU's own modes: nothing for the host
        } else {
            give_up(std::string(name));
        }
    }
    static std::string relation(std::string_view two) {
        if (two == "eq")
            return "==";
        if (two == "lg" || two == "ne")
            return "!=";
        if (two == "gt")
            return ">";
        if (two == "ge")
            return ">=";
        if (two == "lt")
            return "<";
        if (two == "le")
            return "<=";
        give_up("comparison " + std::string(two));
    }
    void emit_sopc(const Ins &ins) {
        const auto name = ins.name;
        const auto a = operand(ins, ins.w0 & 0xff), b = operand(ins, (ins.w0 >> 8) & 0xff);
        if (starts(name, "s_cmp_")) {
            const bool is_signed = ends(name, "_i32");
            line("scc = " + (is_signed ? "(int)" + a : a) + " " + relation(name.substr(6, 2)) +
                 " " + (is_signed ? "(int)" + b : b) + ";");
        } else if (name == "s_bitcmp0_b32") {
            line("scc = (" + a + " & (1u << (" + b + " & 31u))) == 0u;");
        } else if (name == "s_bitcmp1_b32") {
            line("scc = (" + a + " & (1u << (" + b + " & 31u))) != 0u;");
        } else if (name == "s_bitcmp0_b64" || name == "s_bitcmp1_b64") {
            line("scc = ((((" + b + " & 32u) != 0u ? " + operand_high(ins, ins.w0 & 0xff) + " : " +
                 a + ") >> (" + b + " & 31u)) & 1u) " + (name == "s_bitcmp0_b64" ? "==" : "!=") +
                 " 0u;");
        } else {
            give_up(std::string(name));
        }
    }

    // The descriptors of a table are 32 bytes each, but for samplers, which are 16.
    std::uint32_t descriptor_size(std::uint32_t parameter) const {
        const auto &ranges = parameters_[parameter].ranges;
        return !ranges.empty() && ranges[0].type == 3 ? 16 : 32;
    }
    // what a register that an instruction takes a resource from stands for
    Resource resource_of(std::uint32_t reg) {
        if (reg >= 128)
            give_up("a resource that is not a descriptor of a table");
        const auto &known = known_[reg];
        if (known.kind == Known::descriptor && known.several)
            throw Choose{known.choice - 1};
        if (known.kind == Known::root && known.part == 0) {
            // a buffer descriptor the shader made from the address of a root descriptor
            const auto &parameter = parameters_[known.value];
            Resource resource{parameter.type == 2   ? 2u
                              : parameter.type == 3 ? 0u
                                                    : 1u,
                              parameter.reg, parameter.space};
            resource.root = true;
            if (reg + 1 < 128 && known_[reg + 1].kind == Known::root &&
                known_[reg + 1].value == known.value)
                resource.stride = known_[reg + 1].stride;
            return resource;
        }
        if (known.kind == Known::stream && known.part == 0) {
            Resource resource{0u, known.value, 0u};
            resource.stream = true;
            return resource;
        }
        if (known.kind != Known::descriptor || known.part != 0)
            give_up("a resource that is not a descriptor of a table");
        const auto &ranges = parameters_[known.value].ranges;
        if (shader_.heaps) {
            Resource resource;
            resource.heap = true;
            resource.parameter = known.value;
            resource.origin = known.origin;
            resource.dynamic = known.dynamic;
            if (const auto whole = heap_parameter_.find(known.value);
                whole != heap_parameter_.end()) {
                // one of a heap itself, by its byte offset there, which may be known now
                resource.type = whole->second == 2                   ? 3u
                                : facts_.written.count(known.origin) ? 1u
                                                                     : 0u;
                if (known.dynamic < 0) {
                    resource.reg = known.offset / (whole->second == 2 ? 16 : 32);
                    resource.loaded = true;
                }
                return resource;
            }
            if (known.dynamic >= 0)
                give_up("a descriptor of a table picked while the shader runs");
            const auto in = table_register_.find(known.value);
            for (const auto &bound : bound_)
                if (bound.offset == known.offset && in != table_register_.end() &&
                    bound.reg == in->second) {
                    if (bound.type == 3)
                        give_up("a sampler of a table");
                    resource.type = bound.type;
                    resource.reg = known.offset / 32;
                    resource.loaded = true;
                    return resource;
                }
            give_up("a descriptor its program's records do not name");
        }
        if (known.dynamic < 0) {
            const auto index = known.offset / descriptor_size(known.value);
            for (const auto &range : ranges)
                if (index >= range.offset &&
                    (range.count == ~0u || index - range.offset < range.count)) {
                    Resource resource{range.type, range.base + (index - range.offset), range.space};
                    resource.loaded = true;
                    resource.origin = known.origin;
                    return resource;
                }
            give_up("a descriptor outside its table");
        }
        // One of many, picked while the shader runs. A table that has views of both kinds
        // at the same place leaves it to the shader which one a descriptor is: one it
        // writes through is an unordered access view.
        Resource resource;
        resource.dynamic = known.dynamic;
        resource.parameter = known.value;
        resource.origin = known.origin;
        resource.loaded = true;
        bool readable = false, writable = false;
        for (const auto &range : ranges) {
            readable |= range.type == 0;
            writable |= range.type == 1;
        }
        if (ranges.empty())
            give_up("a descriptor outside its table");
        resource.type =
            readable && writable ? (facts_.written.count(known.origin) ? 1u : 0u) : ranges[0].type;
        return resource;
    }
    // A sampler. An Xbox One shader has the words of a static one in its code, and the
    // record of what it binds with the same words says which register that is.
    Resource sampler_of(std::uint32_t reg) {
        // a cached program's: the one sampler its pipeline's signature has (what the words
        // say of filtering and addressing is not read yet)
        if (shader_.heaps && reg + 2 < 128 && known_[reg].kind == Known::constant)
            return Resource{3, 0, 200};
        if (!older_ || reg + 2 >= 128 || known_[reg].kind != Known::constant ||
            known_[reg + 1].kind != Known::constant || known_[reg + 2].kind != Known::constant)
            return resource_of(reg);
        const auto records = shader_.bindings;
        for (std::size_t at = 8; at + 56 <= records.size(); at += 56)
            if ((word(records, at) & 0xff) == 4 && word(records, at + 24) == known_[reg].value &&
                word(records, at + 28) == known_[reg + 1].value &&
                word(records, at + 32) == known_[reg + 2].value)
                return Resource{3, word(records, at + 8), word(records, at + 12)};
        give_up("a sampler made in the code that the shader does not say it binds");
    }
    // a resource the shader writes through, which for one picked at run time settles what
    // it is declared as
    void written(const Resource &resource) {
        if (resource.type == 1)
            return;
        // one of the heap itself is whatever the shader does with it
        if (resource.heap && (resource.dynamic >= 0 || heap_parameter_.count(resource.parameter)) &&
            resource.type == 0 && facts_.written.insert(resource.origin).second)
            throw Retry{};
        if (resource.dynamic >= 0 && !facts_.written.count(resource.origin)) {
            for (const auto &range : parameters_[resource.parameter].ranges)
                if (range.type == 1) {
                    facts_.written.insert(resource.origin);
                    throw Retry{};
                }
        }
        give_up(std::string("a store to something that is not an unordered access view (") +
                (resource.root ? "a root view"
                 : resource.heap
                     ? (heap_parameter_.count(resource.parameter) ? "of the heap" : "of a table")
                 : resource.loaded ? "of a table's ranges"
                                   : "bound") +
                ", type " + std::to_string(resource.type) + ")");
    }
    // The name of a resource as the host's shader has it, declared the first time as `type`.
    // One picked at run time is an element of an array over a range of its table: each type
    // gets a range of its own, which is what the table has several for.
    std::string object(const Resource &resource, const std::string &type) {
        const char letter = "tubs"[resource.type];
        if (resource.loaded) {
            const auto [known, added] = facts_.types.emplace(resource.origin, type);
            if (!added && known->second != type && resource.dynamic < 0)
                throw Unsupported{"a descriptor as " + known->second + " and as " + type, ~0u,
                                  true};
        }
        if (resource.heap && resource.dynamic >= 0) {
            // by its byte offset in the heap, which is known where it is used
            const auto name = "h" + std::to_string(temporaries_++);
            uses_heap_ = needs_sm6_ = true;
            line(type + " " + name + " = " +
                 (resource.type == 3 ? "SamplerDescriptorHeap[d" : "ResourceDescriptorHeap[d") +
                 std::to_string(resource.dynamic) + (resource.type == 3 ? " >> 4];" : " >> 5];"));
            return name;
        }
        if (resource.heap && heap_parameter_.count(resource.parameter)) {
            // a descriptor of a heap itself at a place known now
            const auto name = std::string("hh") + letter + std::to_string(resource.reg);
            const auto found = declared_.find(name);
            if (found != declared_.end()) {
                if (found->second != type)
                    throw Unsupported{name + " as " + found->second + " and as " + type, ~0u, true};
                return name;
            }
            declared_[name] = type;
            uses_heap_ = needs_sm6_ = true;
            prologue_ +=
                "    " + type + " " + name + " = " +
                (resource.type == 3 ? "SamplerDescriptorHeap[" : "ResourceDescriptorHeap[") +
                std::to_string(resource.reg) + "u];\n";
            return name;
        }
        if (resource.heap) {
            const auto slot = table_register_[resource.parameter] - user_first();
            const auto name = std::string("h") + letter + std::to_string(slot) + "_" +
                              std::to_string(resource.reg);
            const auto found = declared_.find(name);
            if (found != declared_.end()) {
                if (found->second != type)
                    throw Unsupported{name + " as " + found->second + " and as " + type, ~0u, true};
                return name;
            }
            declared_[name] = type;
            uses_heap_ = needs_sm6_ = true;
            prologue_ += "    " + type + " " + name + " = ResourceDescriptorHeap[xv_ud" +
                         std::to_string(slot) + " + " + std::to_string(resource.reg) + "u];\n";
            return name;
        }
        if (resource.dynamic < 0) {
            // the sampler a shader makes of words in its code: the host signature's one
            // that compares where the shader compares with it
            auto reg = resource.reg;
            if (resource.type == 3 && resource.space == 200 && type == "SamplerComparisonState")
                reg = 1;
            const auto name =
                std::string(1, letter) + std::to_string(reg) + "_" + std::to_string(resource.space);
            const auto found = declared_.find(name);
            if (found != declared_.end()) {
                if (found->second != type)
                    throw Unsupported{name + " as " + found->second + " and as " + type, ~0u, true};
                return name;
            }
            declared_[name] = type;
            declarations_ += type + " " + name + " : register(" + letter + std::to_string(reg) +
                             ", space" + std::to_string(resource.space) + ");\n";
            return name;
        }
        const auto &ranges = parameters_[resource.parameter].ranges;
        const auto key = std::to_string(resource.parameter) + letter + type;
        auto found = arrays_.find(key);
        if (found == arrays_.end()) {
            std::size_t chosen = ranges.size();
            for (std::size_t r = 0; r < ranges.size() && chosen == ranges.size(); ++r)
                if (ranges[r].type == resource.type && !taken_.count({resource.parameter, r}))
                    chosen = r;
            if (chosen == ranges.size())
                give_up(
                    "more kinds of resources picked at run time than their table has ranges for");
            taken_.insert({resource.parameter, chosen});
            const auto &range = ranges[chosen];
            const auto name = std::string(1, letter) + std::to_string(range.base) + "_" +
                              std::to_string(range.space);
            if (declared_.count(name))
                give_up(name + " as " + declared_[name] + " and as an array of " + type);
            declared_[name] = type + "[]";
            declarations_ += type + " " + name + "[" +
                             (range.count == ~0u ? "" : std::to_string(range.count)) +
                             "] : register(" + letter + std::to_string(range.base) + ", space" +
                             std::to_string(range.space) + ");\n";
            found = arrays_.emplace(key, std::pair{name, range.offset}).first;
        }
        return found->second.first + "[" + slot(resource, found->second.second) + "]";
    }
    std::string slot(const Resource &resource, std::uint32_t first) {
        return "(d" + std::to_string(resource.dynamic) + " / " +
               std::to_string(descriptor_size(resource.parameter)) + "u - " +
               std::to_string(first) + "u)";
    }
    void emit_smem(const Ins &ins) {
        const auto data = (ins.w0 >> 6) & 0x7f, base = (ins.w0 & 0x3f) * 2;
        const auto offset = ins.w1 & 0x1fffff, index = ins.w1 >> 25;
        const bool indexed = index != 0x7d;
        const auto name = ins.name;
        const auto words_of = [&](std::string_view prefix) -> std::uint32_t {
            const auto rest = name.substr(prefix.size());
            return rest.empty() ? 1 : rest == "x2" ? 2 : rest == "x4" ? 4 : rest == "x8" ? 8 : 16;
        };
        std::string where = hex(offset);
        if (indexed)
            where = "(" + operand(ins, index) + " + " + where + ")";
        // Several words may go to the registers their address is in: it is kept before the
        // first is written.
        if (starts(name, "s_load_dword")) {
            const auto words = words_of("s_load_dword");
            const auto from = known_[base];
            if (from.kind == Known::code) {
                // constants kept with the code, after its last instruction
                uses_data_ = true;
                auto address = s(base) + " + " + where;
                if (words > 1)
                    address = keep(address);
                for (std::uint32_t n = 0; n < words; ++n)
                    set_s(data + n, "code_word(" + address + " + " + std::to_string(4 * n) + "u)");
                return;
            }
            if (from.kind == Known::root && from.part == 0) {
                // read through the address of a root descriptor: the buffer's own words
                auto resource = resource_of(base);
                if (indexed && words > 1)
                    where = keep(where);
                for (std::uint32_t n = 0; n < words; ++n)
                    set_s(data + n,
                          read_word(resource, "0u", where, indexed ? ~0u : offset, 4 * n));
                return;
            }
            if (from.kind == Known::memory) {
                // root parameters: constants have their values, the others stand for
                // themselves
                if (indexed)
                    give_up("a root parameter picked while the shader runs");
                for (std::uint32_t n = 0; n < words; ++n) {
                    const auto *const placed = in_memory(offset / 4 + n);
                    if (!placed)
                        set_s(data + n, "0u");
                    else if (parameters_[placed->parameter].type == 1)
                        set_s(data + n,
                              root_constant(placed->parameter, offset / 4 + n - placed->at));
                }
                return;
            }
            if (from.kind == Known::streams)
                return; // a vertex stream's descriptor, which stands for the stream
            if (from.kind != Known::table) {
                static constexpr const char *kinds[] = {
                    "nothing known", "a constant",     "a table",   "a descriptor",
                    "a root view",   "its own code",   "conflicts", "lanes",
                    "root memory",   "vertex streams", "a stream"};
                give_up(
                    std::string("a load from memory that is not a descriptor of a table (from ") +
                    kinds[from.kind] + (from.maybe ? ", on some ways" : "") + ")");
            }
            // where in the table is only known when the shader runs
            if (indexed)
                line("d" + std::to_string(dynamic_of(ins)) + " = " + where + ";");
            else if (facts_.met.count(ins.at))
                line("c" + std::to_string(facts_.first_of(ins.at)) + " = " +
                     std::to_string(ins.at) + "u;");
            return;
        }
        if (starts(name, "s_buffer_load_dword")) {
            const auto words = words_of("s_buffer_load_dword");
            const auto resource = resource_of(base);
            if (indexed && words > 1)
                where = keep(where);
            for (std::uint32_t n = 0; n < words; ++n)
                set_s(data + n, read_word(resource, "0u", where, indexed ? ~0u : offset, 4 * n));
            return;
        }
        if (name == "s_gl1_inv" || name == "s_dcache_inv" || name == "s_dcache_wb")
            return;
        give_up(std::string(name));
    }
    // A word of a buffer: `offset` bytes into element `index`, plus `more`. `fixed` is the
    // offset when it is known now, and ~0 otherwise.
    std::string read_word(const Resource &resource, const std::string &index,
                          const std::string &offset, std::uint32_t fixed, std::uint32_t more) {
        const auto at = "(" + offset + " + " + std::to_string(more) + "u)";
        if (resource.type == 3)
            give_up("a buffer read through a sampler");
        if (resource.type == 2) {
            const auto name = constants(resource);
            // as many vectors as the shader is seen to read: the host's compiler is slow
            // with large ones
            auto &size = constants_size_[name];
            if (index != "0u") {
                // an element of a constant buffer is one of its vectors
                size = 4096;
                const auto place = "(" + index + " * 16u + " + at + ")";
                return name + "[" + place + " >> 4][(" + place + " >> 2) & 3u]";
            }
            size = std::max(size, fixed == ~0u ? 4096u : (fixed + more) / 16 + 1);
            return name + "[" + at + " >> 4][(" + at + " >> 2) & 3u]";
        }
        if (resource.root) {
            const auto name =
                object(resource, resource.type == 1 ? "RWByteAddressBuffer" : "ByteAddressBuffer");
            return name + ".Load(" + bytes_of(resource, index) + at + ")";
        }
        return words(resource) + "[" + index + "].w[" + field(at, fixed, more) + "]";
    }
    // A buffer of the title's as the host's shader sees it: elements of words. The host's
    // view has the stride the title's descriptor gives, so an element is found without the
    // shader knowing its size, and an offset past an element's end reads on into the next,
    // as it does on the console.
    std::string words(const Resource &resource) {
        uses_words_ = true;
        return object(resource,
                      resource.type == 1 ? "RWStructuredBuffer<Words>" : "StructuredBuffer<Words>");
    }
    static constexpr std::uint32_t words_in_element = 512;
    std::string field(const std::string &at, std::uint32_t fixed, std::uint32_t more) {
        if (fixed == ~0u)
            return at + " >> 2";
        if (fixed + more >= 4 * words_in_element)
            give_up("an offset beyond what an element of a buffer is declared with");
        return std::to_string((fixed + more) / 4);
    }
    void write_word(const Resource &resource, const std::string &index, const std::string &offset,
                    std::uint32_t fixed, std::uint32_t more, const std::string &value) {
        written(resource);
        const auto at = "(" + offset + " + " + std::to_string(more) + "u)";
        const std::string guard = exec_changes_ ? "if " + active() + " " : "";
        if (resource.root)
            line(guard + object(resource, "RWByteAddressBuffer") + ".Store(" +
                 bytes_of(resource, index) + at + ", " + value + ");");
        else
            line(guard + words(resource) + "[" + index + "].w[" + field(at, fixed, more) +
                 "] = " + value + ";");
    }
    // the bytes before element `index` of a root buffer, whose stride only the shader has
    std::string bytes_of(const Resource &resource, const std::string &index) {
        if (index == "0u")
            return "";
        if (!resource.stride)
            give_up("a buffer whose stride is not known");
        return index + " * " + std::to_string(resource.stride) + "u + ";
    }
    // a constant buffer as an array of vectors, by name
    std::string constants(const Resource &resource) {
        if (resource.dynamic >= 0 || resource.heap) {
            uses_constants_ = true;
            return object(resource, "ConstantBuffer<Constants>") + ".w";
        }
        const auto name = "b" + std::to_string(resource.reg) + "_" + std::to_string(resource.space);
        if (!declared_.count(name)) {
            declared_[name] = "cbuffer";
            declarations_ += "cbuffer " + name + "_buffer : register(b" +
                             std::to_string(resource.reg) + ", space" +
                             std::to_string(resource.space) + ") { uint4 " + name + "[@" + name +
                             "@]; };\n";
        } else if (declared_[name] != "cbuffer") {
            give_up(name + " as " + declared_[name] + " and as a constant buffer");
        }
        return name;
    }

    void fetch_inputs() {
        const auto &inputs = shader_.inputs;
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const auto &element = inputs[i];
            const auto field = "input.f" + std::to_string(i);
            if (!_stricmp(element.name.c_str(), "SV_VertexID")) {
                line(v(0) + " = " + field + ";");
                vertex_id_ = true;
                continue;
            }
            if (!_stricmp(element.name.c_str(), "SV_InstanceID")) {
                line(v(3) + " = " + field + ";");
                instance_id_ = true;
                continue;
            }
            // element of register r: its components in v[4 + 4r] and on; what it lacks
            // reads 0, 0, 0, 1
            std::uint32_t component = 0;
            for (std::uint32_t channel = 0; channel < 4; ++channel) {
                const auto reg = 4 + 4 * element.reg + channel;
                if (reg > 255)
                    give_up("too many inputs");
                if (element.mask >> channel & 1) {
                    const auto name =
                        field + (element.width > 1 ? std::string(".") + "xyzw"[component] : "");
                    // a signed 10-bit field the host hands over as an unsigned one
                    const bool packed = i < 32 && shader_.signed_inputs >> i & 1 &&
                                        element.type == 3 && component < 3;
                    line(v(reg) + " = " +
                         (packed              ? u("snorm10(" + name + ")")
                          : element.type == 3 ? u(name)
                                              : "(uint)" + name) +
                         ";");
                    ++component;
                } else if (element.mask ==
                               (element.mask & -element.mask) * ((1u << element.width) - 1) &&
                           channel >= static_cast<std::uint32_t>(std::countr_zero(element.mask)) +
                                          element.width) {
                    line(v(reg) + " = " +
                         (channel == 3 && element.type == 3 ? "0x3f800000u" : "0u") + ";");
                }
            }
        }
        if (!vertex_id_)
            line(v(0) + " = input.xv_vertex;");
        if (!instance_id_)
            line(v(3) + " = input.xv_instance;");
        fetched_ = true;
    }
    static bool sixteen(const Element &element) { return element.type >= 4 && element.type <= 6; }
    static bool sixteen_at(const std::vector<Element> &elements, std::uint32_t reg) {
        for (const auto &element : elements)
            if (element.reg == reg)
                return sixteen(element);
        return false;
    }
    // Registers of 16-bit components are packed into one attribute, two components a word:
    // a register starts at a word of its own, components x and y in the first, z and w in
    // the next, and the 16-bit registers after it go on in the words left while they fit,
    // unless one has an attribute of its own (`own`). Each half of the attribute's four
    // words, the field and the component in it there; -1 for a half nothing is in.
    static std::array<std::pair<int, int>, 8> packed(const std::vector<Element> &elements,
                                                     std::uint32_t first,
                                                     const std::set<std::uint32_t> &own) {
        std::array<std::pair<int, int>, 8> halves;
        halves.fill({-1, 0});
        std::uint32_t base = 0;
        for (auto reg = first;
             base < 8 && sixteen_at(elements, reg) && (reg == first || !own.count(reg)); ++reg) {
            std::uint32_t mask = 0;
            for (const auto &element : elements)
                if (element.reg == reg)
                    mask |= element.mask;
            const auto size = static_cast<std::uint32_t>(std::bit_width(mask) + 1) & ~1u;
            if (reg != first && base + size > 8)
                break;
            for (std::size_t field = 0; field < elements.size(); ++field) {
                const auto &element = elements[field];
                if (element.reg != reg)
                    continue;
                for (std::uint32_t c = 0; c < 4 && base + c < 8; ++c)
                    if (element.mask >> c & 1)
                        halves[base + c] = {
                            static_cast<int>(field),
                            std::popcount(static_cast<unsigned>(element.mask & ((1u << c) - 1)))};
            }
            base += size;
        }
        return halves;
    }
    // the registers of a stage's outputs or inputs that another 16-bit one before it packs
    static std::set<std::uint32_t> packed_after(const std::vector<Element> &elements,
                                                const std::set<std::uint32_t> &own) {
        std::set<std::uint32_t> after;
        std::set<std::uint32_t> registers;
        for (const auto &element : elements)
            if (!element.system_value)
                registers.insert(element.reg);
        for (const auto reg : registers) {
            if (after.count(reg) || !sixteen_at(elements, reg))
                continue;
            for (const auto &[field, component] : packed(elements, reg, own))
                if (field >= 0 && elements[field].reg != reg)
                    after.insert(elements[field].reg);
        }
        return after;
    }
    // half `half` of an attribute from register `first` of the input, as bits
    std::string input_half(std::uint32_t first, std::uint32_t half) {
        std::set<std::uint32_t> own;
        for (const auto &[attribute, reg] : attributes_)
            own.insert(reg);
        const auto [field, component] = packed(shader_.inputs, first, own)[half];
        if (field < 0)
            return "0u";
        const auto &element = shader_.inputs[field];
        const auto name = "input.f" + std::to_string(field) +
                          (element.width > 1 ? std::string(".") + "xyzw"[component] : "");
        return element.type == 6 ? "f32tof16(" + name + ")" : "((uint)" + name + " & 0xffffu)";
    }
    // the input register attribute `attribute` starts at
    std::uint32_t attribute_register(std::uint32_t attribute) {
        if (const auto found = attributes_.find(attribute); found != attributes_.end())
            return found->second;
        // without the table, attribute n is the nth register that is not a system value's
        // nor packed with one before it
        const auto after = packed_after(shader_.inputs, {});
        std::vector<std::uint32_t> registers;
        for (const auto &element : shader_.inputs)
            if (!element.system_value && !after.count(element.reg) &&
                (registers.empty() || registers.back() != element.reg))
                registers.push_back(element.reg);
        if (attribute >= registers.size())
            give_up("an attribute the signature does not have");
        return registers[attribute];
    }
    // VOP3's interpolations of a 16-bit half: v_interp_p1ll_f16, v_interp_p1lv_f16 and
    // v_interp_p2_f16 (the attribute in bits 5:0 of the second word, its channel in 7:6 and
    // the half in 8). Both steps leave the interpolated half in the low half of the register.
    void emit_half_interpolation(const Ins &ins) {
        if (stage_ != Stage::pixel)
            give_up("interpolation outside a pixel shader");
        const auto attribute = ins.w1 & 0x3f, channel = (ins.w1 >> 6) & 3, high = (ins.w1 >> 8) & 1;
        const auto reg = attribute_register(attribute);
        set_v(ins.w0 & 0xff, sixteen_at(shader_.inputs, reg)
                                 ? input_half(reg, channel * 2 + high)
                                 : "(" + input_channel(reg, channel) + " >> " +
                                       std::to_string(16 * high) + " & 0xffffu)");
    }
    // component `channel` of the input in register `reg`, as bits
    std::string input_channel(std::uint32_t reg, std::uint32_t channel) {
        if (sixteen_at(shader_.inputs, reg))
            return "(" + input_half(reg, channel * 2) + " | " + input_half(reg, channel * 2 + 1) +
                   " << 16)";
        const auto &inputs = shader_.inputs;
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const auto &element = inputs[i];
            if (element.reg != reg || !(element.mask >> channel & 1))
                continue;
            const auto component =
                std::popcount(static_cast<unsigned>(element.mask & ((1u << channel) - 1)));
            const auto name = "input.f" + std::to_string(i) +
                              (element.width > 1 ? std::string(".") + "xyzw"[component] : "");
            return element.type == 3 ? u(name) : "(uint)" + name;
        }
        return "0u";
    }
    void emit_interpolation(const Ins &ins) {
        if (stage_ != Stage::pixel)
            give_up("interpolation outside a pixel shader");
        const auto destination = (ins.w0 >> 18) & 0xff, attribute = (ins.w0 >> 10) & 0x3f,
                   channel = (ins.w0 >> 8) & 3;
        // the two steps of an interpolation both leave the interpolated value
        set_v(destination, input_channel(attribute_register(attribute), channel));
    }

    // What lane `perm[i]` of each quad holds, for lane i of it, as a variable. A pixel shader
    // reads its quad's lanes to take differences between neighbouring pixels: the console's
    // lanes 0 to 3 of a quad are the top left, top right, bottom left and bottom right
    // pixels, as the host's quad indices are. The read is not under the guard of a lane's
    // EXEC bit, so that the lanes it reads take part in it as on the console, where it
    // reads the registers of inactive lanes too.
    std::string quad_read(const std::string &source, const std::array<std::uint32_t, 4> &perm) {
        needs_sm6_ = true;
        const auto id = "x" + std::to_string(temporaries_++);
        const auto is = [&](std::array<std::uint32_t, 4> other) { return perm == other; };
        if (is({0, 1, 2, 3}))
            line("uint " + id + " = " + source + ";");
        else if (is({1, 0, 3, 2}))
            line("uint " + id + " = QuadReadAcrossX(" + source + ");");
        else if (is({2, 3, 0, 1}))
            line("uint " + id + " = QuadReadAcrossY(" + source + ");");
        else if (is({3, 2, 1, 0}))
            line("uint " + id + " = QuadReadAcrossDiagonal(" + source + ");");
        else if (perm[0] == perm[1] && perm[1] == perm[2] && perm[2] == perm[3])
            line("uint " + id + " = QuadReadLaneAt(" + source + ", " + std::to_string(perm[0]) +
                 "u);");
        else {
            // this lane's place in its quad: the index whose lane has its own number
            line("uint " + id + "s = " + source + ", " + id + "n = WaveGetLaneIndex();");
            line("uint " + id + "i = " + id + "n == QuadReadLaneAt(" + id + "n, 1u) ? 1u : " + id +
                 "n == QuadReadLaneAt(" + id + "n, 2u) ? 2u : " + id + "n == QuadReadLaneAt(" + id +
                 "n, 3u) ? 3u : 0u;");
            for (std::uint32_t k = 0; k < 4; ++k)
                line("uint " + id + "q" + std::to_string(k) + " = QuadReadLaneAt(" + id + "s, " +
                     std::to_string(perm[k]) + "u);");
            line("uint " + id + " = " + id + "i < 2u ? (" + id + "i == 0u ? " + id + "q0 : " + id +
                 "q1) : (" + id + "i == 2u ? " + id + "q2 : " + id + "q3);");
        }
        return id;
    }
    // A pixel shader's DPP16 instruction that permutes its quads (quad_perm), with every
    // row and bank enabled and no modifier on the lane read: a move, or an operation with
    // the lane read as its first source.
    bool emit_quad_dpp(const Ins &ins) {
        const auto modifier = ins.modifier, control = (modifier >> 8) & 0x1ff;
        if ((ins.w0 & 0x1ff) != 250 || control >= 0x100 || (modifier >> 20 & 0xf) ||
            (modifier >> 24 & 0xff) != 0xff || (ins.format != F::vop1 && ins.format != F::vop2))
            return false;
        const auto name = ins.name;
        if (name != "v_mov_b32" && ins.format == F::vop1)
            return false;
        const std::array<std::uint32_t, 4> perm{control & 3, (control >> 2) & 3, (control >> 4) & 3,
                                                (control >> 6) & 3};
        const auto destination = (ins.w0 >> 17) & 0xff;
        const auto a = quad_read(v(modifier & 0xff), perm);
        if (name == "v_mov_b32")
            set_v(destination, a);
        else
            set_v(destination, binary(name, a, v((ins.w0 >> 9) & 0xff), v(destination)));
        return true;
    }

    void emit_dpp8_move(const Ins &ins) {
        if (emit_quad_dpp(ins))
            return;
        if (stage_ != Stage::pixel || ins.name != "v_mov_b32" || (ins.w0 & 0x1ff) != 233)
            give_up("an instruction that reads another lane");
        if (exec_changes_ || sopk_writes_exec_ || blocks_.size() != 1 || blocks_[0].opens)
            give_up("a DPP8 move with control flow or changes to EXEC");

        std::array<std::uint32_t, 4> lanes{};
        for (std::uint32_t lane = 0; lane < lanes.size(); ++lane) {
            lanes[lane] = (ins.modifier >> (8 + 3 * lane)) & 7;
            const auto next_quad = (ins.modifier >> (20 + 3 * lane)) & 7;
            if (lanes[lane] >= 4 || next_quad != lanes[lane] + 4)
                give_up("a DPP8 move without a repeated quad-local permutation");
        }

        needs_sm6_ = true;
        const auto source = v(ins.modifier & 0xff);
        const auto id = "x" + std::to_string(temporaries_++);
        const auto local = id + "lane";
        const auto input = id + "src";
        line("uint " + local + " = WaveGetLaneIndex() & 3u;");
        // Quad reads can see helper lanes. A normal DPP8 read of an inactive source yields zero.
        line("uint " + input + " = IsHelperLane() ? 0u : " + source + ";");
        for (std::uint32_t lane = 0; lane < lanes.size(); ++lane) {
            const auto read = id + "q" + std::to_string(lane);
            line("uint " + read + " = QuadReadLaneAt(" + input + ", " +
                 std::to_string(lanes[lane]) + "u);");
        }
        set_v((ins.w0 >> 17) & 0xff, "(" + local + " < 2u ? (" + local + " == 0u ? " + id +
                                         "q0 : " + id + "q1) : (" + local + " == 2u ? " + id +
                                         "q2 : " + id + "q3))");
    }

    // The instructions with one or two sources in one word. A second word may say which
    // byte or half of each source is meant, and which of the destination is written.
    // An instruction whose first source is another lane's register. A compute shader's
    // wave is the one lane it is run for, lane 0: the lanes around it hold nothing, which is
    // what code that sums over a wave has put in the lanes that are not active. Only the
    // operations nothing changes are taken, so that such a sum comes out as this lane's own.
    void emit_lanes(const Ins &ins) {
        const auto name = ins.name;
        const bool known = name == "v_mov_b32" || name == "v_add_nc_u32" || name == "v_or_b32" ||
                           name == "v_xor_b32" || name == "v_max_u32";
        if (stage_ != Stage::compute || !known)
            give_up("an instruction that reads another lane");
        const auto destination = (ins.w0 >> 17) & 0xff, modifier = ins.modifier;
        // own: the lane read is this one; there: it is a lane of the wave at all
        bool own = false, there = true, bound = false, enabled = true;
        if ((ins.w0 & 0x1ff) == 250) {
            const auto control = (modifier >> 8) & 0x1ff;
            bound = modifier >> 19 & 1;
            enabled = (modifier >> 24 & 1) && (modifier >> 28 & 1);
            if (modifier >> 20 & 0xf)
                give_up("an instruction that reads another lane");
            if (control < 0x100) // each lane of four reads one of the four
                own = (control & 3) == 0;
            else if ((control >= 0x111 && control <= 0x11f) || control == 0x138)
                there = false; // moved up from the lane before the first
            else if (control >= 0x150 && control <= 0x16f)
                own = (control & 15) == 0; // one lane for the row, or the lane's number changed
            else if (!((control >= 0x101 && control <= 0x10f) ||
                       (control >= 0x121 && control <= 0x12f) || control == 0x130 ||
                       control == 0x134 || control == 0x13c || control == 0x140 ||
                       control == 0x141))
                give_up("an instruction that reads another lane");
        } else {
            own = ((modifier >> 8) & 7) == 0; // each lane of eight reads one of the eight
        }
        if (!enabled || (!own && !there && !bound))
            return;
        const auto a = own ? v(modifier & 0xff) : std::string("0u");
        if (name == "v_mov_b32")
            set_v(destination, a);
        else
            set_v(destination, binary(name, a, v((ins.w0 >> 9) & 0xff), v(destination)));
    }
    void emit_short(const Ins &ins) {
        if (ins.form == Ins::Form::lanes) {
            // a pixel shader's are moves within a quad; a compute shader's sum over its wave
            if (stage_ == Stage::pixel)
                emit_dpp8_move(ins);
            else
                emit_lanes(ins);
            return;
        }
        const auto destination = (ins.w0 >> 17) & 0xff, modifier = ins.modifier;
        const auto name = ins.name;
        if (name == "v_nop")
            return;
        if (starts(name, "v_movrel")) {
            // A register picked by M0, counted from the one named: of the source
            // (v_movrels), the destination (v_movreld) or both.
            const auto source = ins.w0 & 0x1ff;
            const bool to = name != "v_movrels_b32", from = name != "v_movreld_b32";
            if (name == "v_movrelsd_2_b32" || ins.form != Ins::Form::plain ||
                (from && source < 256))
                give_up(std::string(name));
            const auto last = std::max(to ? destination : 0u, from ? source - 256 : 0u);
            line("switch (" + s(124) + ") {");
            for (std::uint32_t n = 0; n < std::min(256 - last, 128u); ++n) {
                line("case " + std::to_string(n) + "u:");
                set_v(to ? destination + n : destination,
                      from ? v(source - 256 + n) : operand(ins, source));
                line("break;");
            }
            line("}");
            return;
        }
        std::string a, b;
        std::uint32_t mask = 106;
        // SDWA's clamp (bit 13) and output modifier (bits 15:14) of a float result
        std::uint32_t output = 0;
        if (is_wide(name)) {
            if (ins.form == Ins::Form::sub_dword)
                give_up(std::string(name) + " on part of a register");
            const std::uint32_t codes[2] = {ins.w0 & 0x1ff, 256 + ((ins.w0 >> 9) & 0xff)};
            emit_wide(ins, ins.format == F::vopc ? mask : destination, codes, 0, 0);
            return;
        }
        if (ins.form == Ins::Form::sub_dword) {
            const bool half = ends(name, "_f16");
            const auto part = [&](std::uint32_t code, std::uint32_t bits) {
                const auto select = bits & 7;
                auto x = bits >> 7 & 1 ? operand(ins, code) : v(code);
                if (select < 4)
                    x = "((" + x + " >> " + std::to_string(8 * select) + ") & 0xffu)";
                else if (select < 6)
                    x = "((" + x + " >> " + std::to_string(16 * (select - 4)) + ") & 0xffffu)";
                if (bits >> 3 & 1 && select < 6) {
                    const auto shift = std::to_string(select < 4 ? 24 : 16);
                    x = "(uint)(((int)(" + x + " << " + shift + ")) >> " + shift + ")";
                }
                const auto sign = half ? "0x8000u" : "0x80000000u";
                if (bits >> 5 & 1)
                    x = "(" + x + " & ~" + sign + ")";
                if (bits >> 4 & 1)
                    x = "(" + x + " ^ " + sign + ")";
                return x;
            };
            a = part(modifier & 0xff, modifier >> 16);
            if (ins.format != F::vop1)
                b = part((ins.w0 >> 9) & 0xff, modifier >> 24);
            if (ins.format == F::vopc) {
                if (modifier >> 15 & 1)
                    mask = (modifier >> 8) & 0x7f;
            } else {
                output = modifier >> 13 & 7;
                const bool float_result = (name.find("_f32") != std::string_view::npos ||
                                           name.find("_f16") != std::string_view::npos) &&
                                          (!starts(name, "v_cvt_") || starts(name, "v_cvt_f"));
                if (output && (((modifier >> 8) & 7) != 6 || !float_result))
                    give_up(std::string(name) + " on part of a register, with a modifier");
                placed_ = {(modifier >> 8) & 7, (modifier >> 11) & 3};
            }
        } else {
            a = operand(ins, ins.w0 & 0x1ff);
            b = v((ins.w0 >> 9) & 0xff);
        }
        if (ins.format == F::vop1) {
            if (name != "v_nop")
                emit_vop1(name, destination, a);
        } else if (ins.format == F::vop2) {
            emit_vop2(ins, a, b);
        } else {
            emit_compare(name, mask, a, b);
        }
        placed_.reset();
        if (output)
            finish_float(body_.size(), destination, output >> 1, output & 1,
                         starts(name, "v_cvt_") ? starts(name, "v_cvt_f16") : ends(name, "_f16"));
    }
    // a value as the part of a register that an instruction writes
    std::string place(std::uint32_t reg, const std::string &value) {
        const auto [select, unused] = *placed_;
        if (select >= 6)
            return value;
        const std::uint32_t width = select < 4 ? 8 : 16,
                            shift = select < 4 ? 8 * select : 16 * (select - 4);
        const auto part = "((" + value + ") & " + (width == 8 ? "0xffu" : "0xffffu") + ")";
        if (unused == 2) // the rest stays
            return "((" + v(reg) + " & " + hex(~((width == 8 ? 0xffu : 0xffffu) << shift)) +
                   ") | (" + part + " << " + std::to_string(shift) + "))";
        if (unused == 1) // the sign fills what is above
            return "(uint)(((int)(" + part + " << " + std::to_string(32 - width) + ")) >> " +
                   std::to_string(32 - width - shift) + ")";
        return "(" + part + " << " + std::to_string(shift) + ")";
    }
    // The threads of a group wait for each other. The host only allows it where every
    // thread is sure to come by: outside the blocks a branch may skip and outside loops.
    void synchronize() {
        if (indent_.size() == 4)
            line("GroupMemoryBarrierWithGroupSync();");
        shared_ = Shared::settled;
    }
    // A wave runs each instruction for all of its lanes before the next one, so what one
    // lane writes is there for every lane that reads after it, and what one reads is not
    // yet what another writes after it. The host's threads run on their own: they are made
    // to wait for each other where a shader goes from writing to reading or back.
    void touch_shared(bool write) {
        const auto now = write ? Shared::written : Shared::read;
        if (shared_ != Shared::settled && shared_ != now)
            synchronize();
        shared_ = now;
    }
    // the word of the group's memory at `offset` bytes from `address`
    std::string shared_word(const std::string &address, std::uint32_t offset) {
        return "lds[min((" + address + " + " + std::to_string(offset) + "u) >> 2, " +
               std::to_string(shared_words() - 1) + "u)]";
    }
    // LDS_SIZE of COMPUTE_PGM_RSRC2, in units of 128 words; the host has 32 KiB at most
    std::uint32_t shared_words() const {
        // a mesh shader's record has it in bytes
        if (stage_ == Stage::mesh)
            return mesh_shared_ ? std::min((mesh_shared_ + 3) / 4, 7168u) : 7168u;
        const auto units = (header_[0] >> 15) & 0x1ff;
        return units ? std::min(units * 128, 8192u) : 8192u;
    }
    // The memory the threads of a group share (LDS), as an array of words.
    void emit_group_memory(const Ins &ins) {
        auto name = ins.name;
        if (name == "ds_nop")
            return;
        // a mesh shader's group shares its memory as a compute shader's does
        if (stage_ != Stage::compute && stage_ != Stage::mesh)
            give_up(std::string(name) + " outside a compute shader");
        if (ins.w0 >> 16 & 1)
            give_up(std::string(name) + " on global memory");
        const std::uint32_t offsets[2] = {ins.w0 & 0xff, (ins.w0 >> 8) & 0xff};
        const auto offset = ins.w0 & 0xffff;
        const std::uint32_t data[2] = {(ins.w1 >> 8) & 0xff, (ins.w1 >> 16) & 0xff};
        const auto destination = ins.w1 >> 24;
        const std::string guard = exec_changes_ ? "if " + active() + " " : "";
        // the address first: an instruction may write the register it takes it from
        const auto at = "x" + std::to_string(temporaries_++);
        const auto address = [&] { line("uint " + at + " = " + v(ins.w1 & 0xff) + ";"); };
        const std::uint32_t words = ends(name, "_b64")    ? 2
                                    : ends(name, "_b96")  ? 3
                                    : ends(name, "_b128") ? 4
                                                          : 1;
        // _d16 / _d16_hi: lower / upper half of the register, the other half kept
        const bool upper = ends(name, "_d16_hi"), half = upper || ends(name, "_d16");
        if (half)
            name.remove_suffix(upper ? 7 : 4);
        const auto datum = upper ? "(" + v(data[0]) + " >> 16)" : v(data[0]);
        const auto into = [&](const std::string &value) {
            return !half   ? value
                   : upper ? "(" + v(destination) + " & 0xffffu) | (" + value + " << 16)"
                           : "(" + v(destination) + " & 0xffff0000u) | (" + value + " & 0xffffu)";
        };
        if (name == "ds_read_b32" || name == "ds_read_b64" || name == "ds_read_b96" ||
            name == "ds_read_b128") {
            touch_shared(false);
            address();
            for (std::uint32_t n = 0; n < words; ++n)
                set_v(destination + n, shared_word(at, offset + 4 * n));
            return;
        }
        if (name == "ds_write_b32" || name == "ds_write_b64" || name == "ds_write_b96" ||
            name == "ds_write_b128") {
            touch_shared(true);
            address();
            for (std::uint32_t n = 0; n < words; ++n)
                line(guard + shared_word(at, offset + 4 * n) + " = " + v(data[0] + n) + ";");
            return;
        }
        if (starts(name, "ds_read2") || starts(name, "ds_write2")) {
            // two places, each counted in elements, or in 64 of them
            const bool write = starts(name, "ds_write2");
            const auto unit = 4 * words * (name.find("st64") != std::string_view::npos ? 64 : 1);
            touch_shared(write);
            address();
            for (std::uint32_t which = 0; which < 2; ++which)
                for (std::uint32_t n = 0; n < words; ++n) {
                    const auto word = shared_word(at, offsets[which] * unit + 4 * n);
                    if (write)
                        line(guard + word + " = " + v(data[which] + n) + ";");
                    else
                        set_v(destination + which * words + n, word);
                }
            return;
        }
        const auto shift = "(((" + at + " + " + std::to_string(offset) + "u) & 3u) * 8u)";
        if (name == "ds_read_u8" || name == "ds_read_i8" || name == "ds_read_u16" ||
            name == "ds_read_i16") {
            const bool byte = ends(name, "8"), extend = name[8] == 'i';
            touch_shared(false);
            address();
            const auto part = "((" + shared_word(at, offset) + " >> " + shift + ") & " +
                              (byte ? "0xffu" : "0xffffu") + ")";
            const auto bits = byte ? "24" : "16";
            set_v(destination,
                  into(extend ? "(uint)(((int)(" + part + " << " + bits + ")) >> " + bits + ")"
                              : part));
            return;
        }
        if (name == "ds_write_b8" || name == "ds_write_b16") {
            // part of a word, whose other parts another thread may be writing
            const auto mask = name == "ds_write_b8" ? "0xffu" : "0xffffu";
            touch_shared(true);
            address();
            line(guard + "{ InterlockedAnd(" + shared_word(at, offset) + ", ~(" + mask + " << " +
                 shift + ")); InterlockedOr(" + shared_word(at, offset) + ", (" + datum + " & " +
                 mask + ") << " + shift + "); }");
            return;
        }
        if (half)
            give_up(std::string(ins.name));
        // ds_<operation>[_rtn]_<type>: the word takes the operation's result, and with _rtn
        // the destination what the word had
        name.remove_prefix(3);
        const auto type = name.substr(name.rfind('_') + 1);
        name.remove_suffix(type.size() + 1);
        const bool returns = ends(name, "_rtn");
        if (returns)
            name.remove_suffix(4);
        if (returns && (name == "wrxchg2" || name == "wrxchg2st64") && type == "b32") {
            const auto unit = name == "wrxchg2st64" ? 4 * 64 : 4;
            touch_shared(true);
            address();
            // both words are exchanged before either is returned: the first may go to the
            // register the second is taken from
            std::string before[2];
            for (std::uint32_t which = 0; which < 2; ++which) {
                before[which] = "x" + std::to_string(temporaries_++);
                line("uint " + before[which] + " = 0u;");
                line(guard + atomic("swap", shared_word(at, offsets[which] * unit), v(data[which]),
                                    "", before[which]));
            }
            for (std::uint32_t which = 0; which < 2; ++which)
                set_v(destination + which, before[which]);
            return;
        }
        struct Operation {
            std::string_view name, type, how;
            bool two;
        };
        static constexpr Operation operations[] = {
            {"add", "u32", "add", false},      {"sub", "u32", "sub", false},
            {"rsub", "u32", "rsub", false},    {"inc", "u32", "wrapinc", false},
            {"dec", "u32", "wrapdec", false},  {"min", "i32", "smin", false},
            {"max", "i32", "smax", false},     {"min", "u32", "umin", false},
            {"max", "u32", "umax", false},     {"and", "b32", "and", false},
            {"or", "b32", "or", false},        {"xor", "b32", "xor", false},
            {"mskor", "b32", "mskor", true},   {"wrxchg", "b32", "swap", false},
            {"cmpst", "b32", "cmpswap", true}, {"cmpst", "f32", "fcmpswap", true},
            {"min", "f32", "fmin", false},     {"max", "f32", "fmax", false},
            {"add", "f32", "fadd", false},     {"wrap", "b32", "wrap", true}};
        const Operation *found = nullptr;
        for (const auto &operation : operations)
            if (name == operation.name && type == operation.type)
                found = &operation;
        if (!found || (found->how == "swap" && !returns) || (found->how == "wrap" && !returns))
            give_up(std::string(ins.name));
        const auto how = found->how;
        touch_shared(true);
        address();
        const auto before = "x" + std::to_string(temporaries_++);
        line("uint " + before + " = 0u;");
        // ds_cmpst: data0 compared, data1 stored. ds_mskor, ds_wrap: data0 mask or limit
        line(guard + (how == "cmpswap" || how == "fcmpswap"
                          ? atomic(how, shared_word(at, offset), v(data[1]), v(data[0]), before)
                          : atomic(how, shared_word(at, offset), v(data[0]),
                                   found->two ? v(data[1]) : "", before)));
        if (returns)
            set_v(destination, before);
    }
    // Reading a register of another pixel of the same quad: the pixel's own value and how
    // it changes across the quad give it.
    void emit_shared(const Ins &ins) {
        const auto name = ins.name;
        const auto offset = ins.w0 & 0xffff;
        // a swizzle has no data operand: the value it moves is in its address field (LLVM's
        // AMDGPU DS_1A_RET form)
        const auto source = ins.w1 & 0xff, destination = ins.w1 >> 24;
        if (name != "ds_swizzle_b32") {
            emit_group_memory(ins);
            return;
        }
        // The two modes of the offset, as AMD's RDNA ISA guides and LLVM's AMDGPU assembler
        // describe them: bit 15 set, each lane of four takes the lane of its quad that two bits
        // of the low byte name; bit 15 clear, lane ((lane & and) | or) ^ xor of its 32, with
        // the three masks in bits 0, 5 and 10. Other values with bit 15 set are other modes.
        const bool quad = offset >> 15 & 1;
        if (quad && (offset & 0x7f00))
            give_up("ds_swizzle_b32 in a mode other than a quad's or a mask's");
        if (stage_ != Stage::pixel || !quad) {
            emit_swizzle(offset, v(source), destination);
            return;
        }
        const auto id = "x" + std::to_string(temporaries_++);
        if (const auto lane = offset & 3; (offset & 0xff) == lane * 0x55) {
            // Every pixel takes the same one's value, which is how a shader takes
            // differences across its quad. This pixel's own value stands for the first
            // pixel's and the others follow from it, so that the differences are right.
            line("float " + id + " = " + f(v(source)) + ";");
            set_v(destination, u(id + (lane & 1 ? " + ddx_coarse(" + id + ")" : "") +
                                 (lane & 2 ? " + ddy_coarse(" + id + ")" : "")));
            return;
        }
        if (!quad_) {
            std::size_t position = shader_.inputs.size();
            for (std::size_t i = 0; i < shader_.inputs.size(); ++i)
                if (!_stricmp(shader_.inputs[i].name.c_str(), "SV_Position"))
                    position = i;
            if (position == shader_.inputs.size())
                give_up("ds_swizzle_b32 in a shader without its position");
            const auto field = "input.f" + std::to_string(position);
            prologue_ += "    uint quad = ((uint)" + field + ".x & 1u) | (((uint)" + field +
                         ".y & 1u) << 1);\n";
            quad_ = true;
        }
        line("uint " + id + "q = (" + hex(offset & 0xff) + " >> (quad * 2u)) & 3u; float " + id +
             " = " + f(v(source)) + ";");
        set_v(destination,
              u(id + " + ((float)(" + id + "q & 1u) - (float)(quad & 1u)) * ddx_fine(" + id +
                ") + ((float)(" + id + "q >> 1) - (float)(quad >> 1)) * ddy_fine(" + id + ")"));
    }
    // A swizzle anywhere but between the pixels of a quad: a read of the lane of the wave the
    // offset names, within the lane's 32 (a host wave has at least that many lanes where it
    // has 32 or 64, as AMD's and NVIDIA's do). A compute shader takes its wave as the one
    // lane it is run for (see emit_lanes), and its swizzle does too: the lane's own value
    // where lane 0 reads lane 0, else the nothing the lanes around it hold.
    void emit_swizzle(std::uint32_t offset, const std::string &value, std::uint32_t destination) {
        const bool quad = offset >> 15 & 1;
        const auto and_mask = offset & 0x1f, or_mask = (offset >> 5) & 0x1f,
                   xor_mask = (offset >> 10) & 0x1f;
        if (stage_ == Stage::compute) {
            const bool own = quad ? (offset & 3) == 0 : (or_mask ^ xor_mask) == 0;
            set_v(destination, own ? value : std::string("0u"));
            return;
        }
        needs_sm6_ = true;
        const auto id = "x" + std::to_string(temporaries_++);
        const auto lane = id + "lane";
        line("uint " + lane + " = WaveGetLaneIndex();");
        const auto target = quad ? "(" + lane + " & ~3u) | ((" + hex(offset & 0xff) + " >> ((" +
                                       lane + " & 3u) * 2u)) & 3u)"
                                 : "(" + lane + " & ~31u) | (((" + lane + " & " + hex(and_mask) +
                                       ") | " + hex(or_mask) + ") ^ " + hex(xor_mask) + ")";
        line("uint " + id + " = WaveReadLaneAt(" + value + ", " + target + ");");
        set_v(destination, id);
    }
    void emit_vop1(std::string_view name, std::uint32_t destination, const std::string &a) {
        std::string value;
        const auto fa = f(a);
        if (name == "v_nop" || name == "v_clrexcp" || name == "v_pipeflush")
            return;
        if (name == "v_swap_b32") {
            line(std::string(exec_changes_ ? "if " + active() + " " : "") + "{ uint X = " +
                 v(destination) + "; " + v(destination) + " = " + a + "; " + a + " = X; }");
            return;
        }
        if (name == "v_mov_b32")
            value = a;
        else if (name == "v_readfirstlane_b32") {
            set_s(destination & 0x7f, a);
            return;
        } else if (name == "v_cvt_i32_f32")
            value = "(uint)(int)clamp(" + fa + ", -2147483648.0, 2147483520.0)";
        else if (name == "v_cvt_u32_f32")
            value = "(uint)clamp(" + fa + ", 0.0, 4294967040.0)";
        else if (name == "v_cvt_f32_i32")
            value = u("(float)(int)" + a);
        else if (name == "v_cvt_f32_u32")
            value = u("(float)" + a);
        else if (name == "v_cvt_f16_f32")
            value = "f32tof16(" + fa + ")";
        else if (name == "v_cvt_f32_f16")
            value = u("f16tof32(" + a + ")");
        else if (name == "v_cvt_rpi_i32_f32")
            value = "(uint)(int)floor(" + fa + " + 0.5)";
        else if (name == "v_cvt_flr_i32_f32")
            value = "(uint)(int)floor(" + fa + ")";
        else if (name == "v_cvt_off_f32_i4")
            value = u("(float)(((int)(" + a + " << 28)) >> 28) / 16.0");
        else if (name == "v_cvt_f32_ubyte0")
            value = u("(float)(" + a + " & 255u)");
        else if (name == "v_cvt_f32_ubyte1")
            value = u("(float)((" + a + " >> 8) & 255u)");
        else if (name == "v_cvt_f32_ubyte2")
            value = u("(float)((" + a + " >> 16) & 255u)");
        else if (name == "v_cvt_f32_ubyte3")
            value = u("(float)(" + a + " >> 24)");
        else if (name == "v_fract_f32")
            value = u("frac(" + fa + ")");
        else if (name == "v_trunc_f32")
            value = u("trunc(" + fa + ")");
        else if (name == "v_ceil_f32")
            value = u("ceil(" + fa + ")");
        else if (name == "v_rndne_f32")
            value = u("round(" + fa + ")");
        else if (name == "v_floor_f32")
            value = u("floor(" + fa + ")");
        else if (name == "v_exp_f32")
            value = u("exp2(" + fa + ")");
        else if (name == "v_log_f32")
            value = u("log2(" + fa + ")");
        else if (name == "v_rcp_f32" || name == "v_rcp_iflag_f32")
            value = u("(1.0 / " + fa + ")");
        else if (name == "v_rsq_f32")
            value = u("rsqrt(" + fa + ")");
        else if (name == "v_sqrt_f32")
            value = u("sqrt(" + fa + ")");
        else if (name == "v_sin_f32")
            value = u("sin(" + fa + " * 6.28318530718)");
        else if (name == "v_cos_f32")
            value = u("cos(" + fa + " * 6.28318530718)");
        else if (name == "v_not_b32")
            value = "~" + a;
        else if (name == "v_bfrev_b32")
            value = "reversebits(" + a + ")";
        else if (name == "v_ffbh_u32")
            value = "(" + a + " == 0u ? 0xffffffffu : 31u - firstbithigh(" + a + "))";
        else if (name == "v_ffbl_b32")
            value = "firstbitlow(" + a + ")";
        else if (name == "v_ffbh_i32")
            value = "(((int)" + a + " < 0 ? ~" + a + " : " + a +
                    ") == 0u ? 0xffffffffu : 31u - firstbithigh((int)" + a + " < 0 ? ~" + a +
                    " : " + a + "))";
        else if (name == "v_frexp_exp_i32_f32")
            value = "frexp_exponent(" + a + ")";
        else if (name == "v_frexp_mant_f32")
            value = "frexp_mantissa(" + a + ")";
        else if (name == "v_cvt_f16_u16")
            value = "f32tof16((float)(" + a + " & 0xffffu))";
        else if (name == "v_cvt_f16_i16")
            value = "f32tof16((float)(((int)(" + a + " << 16)) >> 16))";
        else if (name == "v_cvt_u16_f16")
            value = "(uint)clamp(f16tof32(" + a + "), 0.0, 65535.0)";
        else if (name == "v_cvt_i16_f16")
            value = "((uint)(int)clamp(f16tof32(" + a + "), -32768.0, 32767.0) & 0xffffu)";
        else if (name == "v_sat_pk_u8_i16")
            value = "((uint)clamp(((int)(" + a + " << 16)) >> 16, 0, 255) | ((uint)clamp((int)" +
                    a + " >> 16, 0, 255) << 8))";
        else if (name == "v_trunc_f16" || name == "v_rndne_f16" || name == "v_fract_f16" ||
                 name == "v_log_f16" || name == "v_exp_f16" || name == "v_sin_f16" ||
                 name == "v_cos_f16") {
            static constexpr std::pair<std::string_view, const char *> functions[] = {
                {"trunc", "trunc(X)"},
                {"rndne", "round(X)"},
                {"fract", "frac(X)"},
                {"log", "log2(X)"},
                {"exp", "exp2(X)"},
                {"sin", "sin(X * 6.28318530718)"},
                {"cos", "cos(X * 6.28318530718)"}};
            for (const auto &[which, function] : functions)
                if (name.substr(2, name.size() - 6) == which)
                    value = function;
            value.replace(value.find('X'), 1, "f16tof32(" + a + ")");
            value = "f32tof16(" + value + ")";
        } else if (name == "v_floor_f16")
            value = "f32tof16(floor(f16tof32(" + a + ")))";
        else if (name == "v_ceil_f16")
            value = "f32tof16(ceil(f16tof32(" + a + ")))";
        else if (name == "v_rcp_f16")
            value = "f32tof16(1.0 / f16tof32(" + a + "))";
        else if (name == "v_sqrt_f16")
            value = "f32tof16(sqrt(f16tof32(" + a + ")))";
        else if (name == "v_rsq_f16")
            value = "f32tof16(rsqrt(f16tof32(" + a + ")))";
        else
            give_up(std::string(name));
        set_v(destination, value);
    }
    // the operations with two sources, shared by the 32-bit and the 64-bit encodings; `a`
    // is the first source, and `d` the destination's own value
    std::string binary(std::string_view name, const std::string &a, const std::string &b,
                       const std::string &d) {
        const auto fa = f(a), fb = f(b), fd = f(d);
        const auto h = [](const std::string &x) { return "f16tof32(" + x + ")"; };
        const auto half = [](const std::string &x) { return "f32tof16(" + x + ")"; };
        if (name == "v_add_f32")
            return u(fa + " + " + fb);
        if (name == "v_sub_f32")
            return u(fa + " - " + fb);
        if (name == "v_subrev_f32")
            return u(fb + " - " + fa);
        if (name == "v_mul_f32")
            return u(fa + " * " + fb);
        if (name == "v_mul_legacy_f32")
            return u("mul_legacy(" + fa + ", " + fb + ")");
        if (name == "v_min_f32")
            return u("min(" + fa + ", " + fb + ")");
        if (name == "v_max_f32")
            return u("max(" + fa + ", " + fb + ")");
        if (name == "v_mac_f32" || name == "v_fmac_f32")
            return u(fa + " * " + fb + " + " + fd);
        if (name == "v_mac_legacy_f32")
            return u("mul_legacy(" + fa + ", " + fb + ") + " + fd);
        if (name == "v_add_f16")
            return half(h(a) + " + " + h(b));
        if (name == "v_sub_f16")
            return half(h(a) + " - " + h(b));
        if (name == "v_subrev_f16")
            return half(h(b) + " - " + h(a));
        if (name == "v_mul_f16")
            return half(h(a) + " * " + h(b));
        if (name == "v_min_f16")
            return half("min(" + h(a) + ", " + h(b) + ")");
        if (name == "v_max_f16")
            return half("max(" + h(a) + ", " + h(b) + ")");
        if (name == "v_fmac_f16")
            return half(h(a) + " * " + h(b) + " + " + h(d));
        if (name == "v_min_i32")
            return "(uint)min((int)" + a + ", (int)" + b + ")";
        if (name == "v_max_i32")
            return "(uint)max((int)" + a + ", (int)" + b + ")";
        if (name == "v_min_u32")
            return "min(" + a + ", " + b + ")";
        if (name == "v_max_u32")
            return "max(" + a + ", " + b + ")";
        if (name == "v_lshrrev_b32")
            return b + " >> (" + a + " & 31u)";
        if (name == "v_ashrrev_i32")
            return "(uint)((int)" + b + " >> (" + a + " & 31u))";
        if (name == "v_lshlrev_b32")
            return b + " << (" + a + " & 31u)";
        if (name == "v_and_b32")
            return a + " & " + b;
        if (name == "v_or_b32")
            return a + " | " + b;
        if (name == "v_xor_b32")
            return a + " ^ " + b;
        if (name == "v_xnor_b32")
            return "~(" + a + " ^ " + b + ")";
        if (name == "v_add_nc_u32")
            return a + " + " + b;
        if (name == "v_sub_nc_u32")
            return a + " - " + b;
        if (name == "v_subrev_nc_u32")
            return b + " - " + a;
        if (name == "v_mul_i32_i24")
            return "(uint)((((int)(" + a + " << 8)) >> 8) * (((int)(" + b + " << 8)) >> 8))";
        if (name == "v_mul_u32_u24")
            return "(" + a + " & 0xffffffu) * (" + b + " & 0xffffffu)";
        if (name == "v_mul_hi_u32_u24")
            return "mul_hi(" + a + " & 0xffffffu, " + b + " & 0xffffffu)";
        if (name == "v_mul_hi_i32_i24")
            return "mul_hi_signed((uint)(((int)(" + a + " << 8)) >> 8), (uint)(((int)(" + b +
                   " << 8)) >> 8))";
        if (name == "v_cvt_pkrtz_f16_f32")
            return "(f32tof16(" + fa + ") | (f32tof16(" + fb + ") << 16))";
        if (name == "v_cvt_pknorm_i16_f32")
            return "(((uint)(int)round(clamp(" + fa +
                   ", -1.0, 1.0) * 32767.0) & 0xffffu) | ((uint)(int)round(clamp(" + fb +
                   ", -1.0, 1.0) * 32767.0) << 16))";
        if (name == "v_cvt_pknorm_u16_f32")
            return "((uint)round(saturate(" + fa + ") * 65535.0) | ((uint)round(saturate(" + fb +
                   ") * 65535.0) << 16))";
        if (name == "v_ldexp_f32")
            return u("ldexp(" + fa + ", (float)(int)" + b + ")");
        if (name == "v_ldexp_f16")
            return half("ldexp(" + h(a) + ", (float)(((int)(" + b + " << 16)) >> 16))");
        // the 16-bit operations take the lower half of each source and leave the upper clear
        const auto low = [](const std::string &x) { return "((" + x + ") & 0xffffu)"; };
        const auto extended = [](const std::string &x) {
            return "(((int)(" + x + " << 16)) >> 16)";
        };
        if (name == "v_add_nc_u16" || name == "v_add_nc_i16")
            return low(a + " + " + b);
        if (name == "v_sub_nc_u16" || name == "v_sub_nc_i16")
            return low(a + " - " + b);
        if (name == "v_mul_lo_u16")
            return low(a + " * " + b);
        if (name == "v_lshlrev_b16")
            return low(b + " << (" + a + " & 15u)");
        if (name == "v_lshrrev_b16")
            return low(b) + " >> (" + a + " & 15u)";
        if (name == "v_ashrrev_i16")
            return low("(uint)(" + extended(b) + " >> (" + a + " & 15u))");
        if (name == "v_min_u16")
            return "min(" + low(a) + ", " + low(b) + ")";
        if (name == "v_max_u16")
            return "max(" + low(a) + ", " + low(b) + ")";
        if (name == "v_min_i16")
            return low("(uint)min(" + extended(a) + ", " + extended(b) + ")");
        if (name == "v_max_i16")
            return low("(uint)max(" + extended(a) + ", " + extended(b) + ")");
        if (name == "v_add_nc_i32")
            return a + " + " + b;
        if (name == "v_sub_nc_i32")
            return a + " - " + b;
        if (name == "v_bcnt_u32_b32")
            return "countbits(" + a + ") + " + b;
        if (name == "v_bfm_b32")
            return "((1u << (" + a + " & 31u)) - 1u) << (" + b + " & 31u)";
        if (name == "v_pack_b32_f16")
            return low(a) + " | (" + b + " << 16)";
        if (name == "v_cvt_pk_u16_u32")
            return "min(" + a + ", 0xffffu) | (min(" + b + ", 0xffffu) << 16)";
        if (name == "v_cvt_pk_i16_i32")
            return low("(uint)clamp((int)" + a + ", -32768, 32767)") + " | ((uint)clamp((int)" + b +
                   ", -32768, 32767) << 16)";
        give_up(std::string(name));
    }
    void emit_vop2(const Ins &ins, const std::string &a, const std::string &b) {
        const auto destination = (ins.w0 >> 17) & 0xff;
        const auto name = ins.name;
        if (name == "v_cndmask_b32") {
            set_v(destination, "(" + s(106) + " != 0u ? " + b + " : " + a + ")");
        } else if (name == "v_fmaak_f32" || name == "v_madak_f32") {
            set_v(destination, u(f(a) + " * " + f(b) + " + " + f(hex(ins.literal))));
        } else if (name == "v_fmamk_f32" || name == "v_madmk_f32") {
            set_v(destination, u(f(a) + " * " + f(hex(ins.literal)) + " + " + f(b)));
        } else if (name == "v_fmamk_f16") {
            set_v(destination, "f32tof16(f16tof32(" + a + ") * f16tof32(" + hex(ins.literal) +
                                   ") + f16tof32(" + b + "))");
        } else if (name == "v_fmaak_f16") {
            set_v(destination, "f32tof16(f16tof32(" + a + ") * f16tof32(" + b + ") + f16tof32(" +
                                   hex(ins.literal) + "))");
        } else if (name == "v_add_co_ci_u32" || name == "v_sub_co_ci_u32" ||
                   name == "v_add_co_u32" || name == "v_sub_co_u32" || name == "v_subrev_co_u32" ||
                   name == "v_subrev_co_ci_u32") {
            carry(name, destination, 106, a, b, s(106));
        } else {
            set_v(destination, binary(name, a, b, v(destination)));
        }
    }
    // an addition or subtraction that leaves its carry in a mask
    void carry(std::string_view name, std::uint32_t destination, std::uint32_t mask,
               const std::string &a, const std::string &b, const std::string &in) {
        const bool subtract = starts(name, "v_sub"), reversed = starts(name, "v_subrev"),
                   with_in = name.find("_ci_") != std::string_view::npos;
        const auto &x = reversed ? b : a, &y = reversed ? a : b;
        const auto extra = with_in ? "(" + in + " != 0u ? 1u : 0u)" : std::string("0u");
        if (subtract)
            line("{ uint cin = " + extra + ", X = " + x + " - " + y + " - cin; bool carried = " +
                 y + " > " + x + " || (cin != 0u && " + y + " == " + x + ");");
        else
            line("{ uint cin = " + extra + ", X = " + x + " + " + y +
                 " + cin; bool carried = X < " + x + " || (cin != 0u && X == " + x + ");");
        set_v(destination, "X");
        set_mask(mask, "carried");
        line("}");
    }
    static bool is_wide(std::string_view name) {
        if (starts(name, "v_cmp"))
            return ends(name, "_f64") || ends(name, "_i64") || ends(name, "_u64");
        for (const auto *wide : {"v_add_f64", "v_mul_f64", "v_min_f64", "v_max_f64", "v_fma_f64",
                                 "v_rcp_f64", "v_cvt_i32_f64", "v_cvt_f64_i32", "v_cvt_f32_f64",
                                 "v_cvt_f64_f32", "v_cvt_u32_f64", "v_cvt_f64_u32"})
            if (name == wide)
                return true;
        return false;
    }
    // {low, high} of a 64-bit operand; a literal is the high word of a double
    std::pair<std::string, std::string> wide_operand(const Ins &ins, std::uint32_t source,
                                                     bool real) {
        if (source >= 256)
            return {v(source - 256), v(source - 255)};
        if (source == 255) {
            if (!real)
                give_up("a literal as a 64-bit integer (" + std::string(ins.name) + ")");
            return {"0u", hex(ins.literal)};
        }
        if (real && source >= 240 && source <= 248) {
            static constexpr double doubles[] = {
                0.5, -0.5, 1.0, -1.0, 2.0, -2.0, 4.0, -4.0, 0.15915494309189532};
            const auto bits = std::bit_cast<std::uint64_t>(doubles[source - 240]);
            return {hex(static_cast<std::uint32_t>(bits)),
                    hex(static_cast<std::uint32_t>(bits >> 32))};
        }
        if ((source >= 128 && source <= 208) || source < 127)
            return {operand(ins, source), operand_high(ins, source)};
        give_up("64-bit operand " + std::to_string(source));
    }
    void emit_wide(const Ins &ins, std::uint32_t destination, std::span<const std::uint32_t> codes,
                   std::uint32_t absolute, std::uint32_t negate) {
        const auto name = ins.name;
        const auto sign = [&](std::size_t n, std::string upper) {
            if (absolute >> n & 1)
                upper = "(" + upper + " & 0x7fffffffu)";
            if (negate >> n & 1)
                upper = "(" + upper + " ^ 0x80000000u)";
            return upper;
        };
        const auto words = [&](std::size_t n, bool real) {
            auto [lower, upper] = wide_operand(ins, codes[n], real);
            return std::pair{lower, sign(n, upper)};
        };
        const auto real = [&](std::size_t n) {
            const auto [lower, upper] = words(n, true);
            return "asdouble(" + lower + ", " + upper + ")";
        };
        const auto narrow = [&](std::size_t n) { return sign(n, operand(ins, codes[n])); };
        const auto set = [&](const std::string &value) {
            const auto id = "x" + std::to_string(temporaries_++);
            line("uint " + id + "l, " + id + "h; asuint(" + value + ", " + id + "l, " + id + "h);");
            set_v(destination, id + "l");
            set_v(destination + 1, id + "h");
        };
        if (starts(name, "v_cmp")) {
            const bool is_real = ends(name, "_f64");
            const auto [lower, upper] = words(0, is_real);
            if (name.find("_class_") != std::string_view::npos) {
                emit_compare(name, destination, lower, operand(ins, codes[1]), upper);
            } else {
                const auto [second, second_upper] = words(1, is_real);
                emit_compare(name, destination, lower, second, upper, second_upper);
            }
        } else if (name == "v_cvt_i32_f64")
            set_v(destination, "(uint)(int)clamp(" + real(0) + ", -2147483648.0L, 2147483647.0L)");
        else if (name == "v_cvt_u32_f64")
            set_v(destination, "(uint)clamp(" + real(0) + ", 0.0L, 4294967295.0L)");
        else if (name == "v_cvt_f32_f64")
            set_v(destination, u("(float)" + real(0)));
        else if (name == "v_cvt_f64_i32")
            set("(double)(int)" + narrow(0));
        else if (name == "v_cvt_f64_u32")
            set("(double)" + narrow(0));
        else if (name == "v_cvt_f64_f32")
            set("(double)" + f(narrow(0)));
        else if (name == "v_rcp_f64")
            set("1.0L / " + real(0));
        else if (name == "v_add_f64")
            set(real(0) + " + " + real(1));
        else if (name == "v_mul_f64")
            set(real(0) + " * " + real(1));
        else if (name == "v_min_f64")
            set("min(" + real(0) + ", " + real(1) + ")");
        else if (name == "v_max_f64")
            set("max(" + real(0) + ", " + real(1) + ")");
        else if (name == "v_fma_f64")
            set("fma(" + real(0) + ", " + real(1) + ", " + real(2) + ")");
        else
            give_up(std::string(name));
    }
    void emit_compare(std::string_view full, std::uint32_t mask, const std::string &a,
                      const std::string &b, const std::string &a_upper = {},
                      const std::string &b_upper = {}) {
        // v_cmp[x]_<relation>_<type>
        const bool kills = starts(full, "v_cmpx_");
        auto rest = full.substr(kills ? 7 : 6);
        const auto split = rest.rfind('_');
        const auto how = rest.substr(0, split), type = rest.substr(split + 1);
        std::string condition;
        if (type == "i64" || type == "u64") {
            const auto less = [&](const std::string &lower, const std::string &upper,
                                  const std::string &than, const std::string &than_upper) {
                return "(" +
                       (type == "i64" ? "(int)" + upper + " < (int)" + than_upper
                                      : upper + " < " + than_upper) +
                       " || (" + upper + " == " + than_upper + " && " + lower + " < " + than + "))";
            };
            if (how == "f")
                condition = "false";
            else if (how == "t")
                condition = "true";
            else if (how == "eq")
                condition = "(" + a + " == " + b + " && " + a_upper + " == " + b_upper + ")";
            else if (how == "ne")
                condition = "(" + a + " != " + b + " || " + a_upper + " != " + b_upper + ")";
            else if (how == "lt")
                condition = less(a, a_upper, b, b_upper);
            else if (how == "gt")
                condition = less(b, b_upper, a, a_upper);
            else if (how == "le")
                condition = "!" + less(b, b_upper, a, a_upper);
            else if (how == "ge")
                condition = "!" + less(a, a_upper, b, b_upper);
            else
                give_up(std::string(full));
        } else if (type == "f64" && how == "class") {
            condition = "double_class(" + a + ", " + a_upper + ", " + b + ")";
        } else if (type == "f32" || type == "f16" || type == "f64") {
            const auto x = type == "f64"   ? "asdouble(" + a + ", " + a_upper + ")"
                           : type == "f16" ? "f16tof32(" + a + ")"
                                           : f(a),
                       y = type == "f64"   ? "asdouble(" + b + ", " + b_upper + ")"
                           : type == "f16" ? "f16tof32(" + b + ")"
                                           : f(b);
            if (how == "lt")
                condition = x + " < " + y;
            else if (how == "eq")
                condition = x + " == " + y;
            else if (how == "le")
                condition = x + " <= " + y;
            else if (how == "gt")
                condition = x + " > " + y;
            else if (how == "lg")
                condition = "(" + x + " < " + y + " || " + x + " > " + y + ")";
            else if (how == "ge")
                condition = x + " >= " + y;
            else if (how == "o")
                condition = "(" + x + " == " + x + " && " + y + " == " + y + ")";
            else if (how == "u")
                condition = "(" + x + " != " + x + " || " + y + " != " + y + ")";
            else if (how == "nge")
                condition = "!(" + x + " >= " + y + ")";
            else if (how == "nlg")
                condition = "!(" + x + " < " + y + " || " + x + " > " + y + ")";
            else if (how == "ngt")
                condition = "!(" + x + " > " + y + ")";
            else if (how == "nle")
                condition = "!(" + x + " <= " + y + ")";
            else if (how == "neq")
                condition = x + " != " + y;
            else if (how == "nlt")
                condition = "!(" + x + " < " + y + ")";
            else if (how == "f")
                condition = "false";
            else if (how == "tru")
                condition = "true";
            else if (how == "class")
                condition = "float_class(" + x + ", " + b + ")";
            else
                give_up(std::string(full));
        } else if (type == "i32" || type == "u32" || type == "i16" || type == "u16") {
            const auto as = [&](const std::string &value) {
                return type == "i32"   ? "(int)" + value
                       : type == "i16" ? "(((int)(" + value + " << 16)) >> 16)"
                       : type == "u16" ? "(" + value + " & 0xffffu)"
                                       : value;
            };
            const auto x = as(a), y = as(b);
            if (how == "f")
                condition = "false";
            else if (how == "t")
                condition = "true";
            else
                condition = x + " " + relation(how) + " " + y;
        } else {
            give_up(std::string(full));
        }
        set_mask(mask, condition);
        if (kills) {
            line(s(126) + " &= " + s(mask) + ";");
            if (wave64_)
                line(s(127) + " = " + s(126) + ";");
        }
    }
    void emit_vop3(const Ins &ins) {
        const auto destination = ins.w0 & 0xff;
        // an addition with a carry has the register its carry goes to where others have
        // their modifiers
        const bool carries = ins.name.find("_co_") != std::string_view::npos;
        const auto absolute = carries ? 0 : (ins.w0 >> 8) & 7, clamp = (ins.w0 >> 15) & 1;
        const auto negate = ins.w1 >> 29, modifier = (ins.w1 >> 27) & 3;
        const std::uint32_t codes[3] = {ins.w1 & 0x1ff, (ins.w1 >> 9) & 0x1ff,
                                        (ins.w1 >> 18) & 0x1ff};
        const auto name = ins.name;
        if (name == "v_interp_p1ll_f16" || name == "v_interp_p1lv_f16" ||
            name == "v_interp_p2_f16") {
            emit_half_interpolation(ins);
            return;
        }
        if (name == "v_writelane_b32" || name == "v_readlane_b32") {
            // A scalar kept in one lane of a vector register and taken back from it: each
            // such lane is a variable of its own.
            const auto lane = constant_of(codes[1], ins.literal);
            const auto variable = [](std::uint32_t slot) {
                return "w" + std::to_string(slot - 128);
            };
            if (name == "v_writelane_b32") {
                const auto slot = lane ? spill(destination, *lane, true) : std::nullopt;
                if (!slot)
                    give_up("v_writelane_b32 to a lane picked at run time");
                if (!(codes[0] < 128 && stands_in(known_[codes[0]]) && !known_[codes[0]].maybe))
                    line(variable(*slot) + " = " + operand(ins, codes[0]) + ";");
                return;
            }
            const auto slot =
                lane && codes[0] >= 256 ? spill(codes[0] - 256, *lane, false) : std::nullopt;
            if (!slot && sums_lanes_ && lane && *lane)
                set_s(destination & 0x7f, "0u"); // a lane around this one: see emit_lanes
            else if (!slot)
                set_s(destination & 0x7f, operand(ins, codes[0]));
            else if (!(stands_in(known_[*slot]) && !known_[*slot].maybe))
                set_s(destination & 0x7f, variable(*slot));
            return;
        }
        if (name == "v_permlane16_b32" || name == "v_permlanex16_b32") {
            // each lane reads one of its row of sixteen, or of the row beside it
            if (stage_ != Stage::compute)
                give_up("an instruction that reads another lane");
            const auto select = constant_of(codes[1], ins.literal);
            set_v(destination, name == "v_permlane16_b32" && select && !(*select & 15)
                                   ? operand(ins, codes[0])
                                   : std::string("0u"));
            return;
        }
        if (name == "v_lshlrev_b64" || name == "v_lshrrev_b64" || name == "v_ashrrev_i64") {
            const auto [low, high] = wide_operand(ins, codes[1], false);
            const auto id = "x" + std::to_string(temporaries_++);
            line("uint " + id + "l, " + id + "h; " +
                 (name == "v_lshlrev_b64"   ? "shift_left"
                  : name == "v_lshrrev_b64" ? "shift_right"
                                            : "shift_signed") +
                 "(" + low + ", " + high + ", " + operand(ins, codes[0]) + ", " + id + "l, " + id +
                 "h);");
            set_v(destination, id + "l");
            set_v(destination + 1, id + "h");
            return;
        }
        if (is_wide(name)) {
            if ((modifier || clamp) && !starts(name, "v_cmp"))
                give_up(std::string(name) + " with a modifier");
            emit_wide(ins, ins.op < 0x100 ? destination & 0x7f : destination, codes, absolute,
                      negate);
            return;
        }
        // which sources each operation has, so that no register is read that is not one
        const bool compare = ins.op < 0x100;
        const bool one = ins.op >= 0x180 && ins.op < 0x200;
        const bool carry_in = name.find("_co_ci_") != std::string_view::npos;
        const bool two =
            compare ||
            (ins.op >= 0x100 && ins.op < 0x140 && name != "v_cndmask_b32" && !carry_in) ||
            name == "v_mul_lo_u32" || name == "v_mul_hi_u32" || name == "v_mul_lo_i32" ||
            name == "v_mul_hi_i32" || name == "v_ldexp_f32" || name == "v_add_co_u32" ||
            name == "v_sub_co_u32" || name == "v_subrev_co_u32" || name == "v_cvt_pkrtz_f16_f32" ||
            name == "v_cvt_pknorm_i16_f32" || name == "v_cvt_pknorm_u16_f32" ||
            name == "v_add_nc_u16" || name == "v_sub_nc_u16" || name == "v_mul_lo_u16" ||
            name == "v_lshlrev_b64" || name == "v_lshrrev_b64" || name == "v_readlane_b32" ||
            name == "v_pack_b32_f16" || name == "v_add_nc_i16" || name == "v_sub_nc_i16" ||
            name == "v_add_nc_i32" || name == "v_sub_nc_i32" || name == "v_lshlrev_b16" ||
            name == "v_lshrrev_b16" || name == "v_ashrrev_i16" || name == "v_min_u16" ||
            name == "v_max_u16" || name == "v_min_i16" || name == "v_max_i16" ||
            name == "v_bcnt_u32_b32" || name == "v_bfm_b32" || name == "v_cvt_pk_u16_u32" ||
            name == "v_cvt_pk_i16_i32" || name == "v_ldexp_f16" || starts(name, "v_mbcnt_");
        const int count = one ? 1 : two ? 2 : 3;
        const bool is_float = name.find("_f32") != std::string_view::npos ||
                              name.find("_f16") != std::string_view::npos;
        const bool is_half = name.find("_f16") != std::string_view::npos &&
                             name.find("f16_f32") == std::string_view::npos;
        std::string x[3];
        for (int n = 0; n < count; ++n) {
            if (name == "v_cndmask_b32" && n == 2) { // the mask: a scalar register
                x[n] = operand(ins, codes[n]);
                continue;
            }
            x[n] = operand(ins, codes[n]);
            if (((absolute >> n & 1) || (negate >> n & 1)) && is_half) {
                // a half's sign is bit 15 of the low half the instruction reads
                if (absolute >> n & 1)
                    x[n] = "(" + x[n] + " & 0xffff7fffu)";
                if (negate >> n & 1)
                    x[n] = "(" + x[n] + " ^ 0x8000u)";
            } else if ((absolute >> n & 1) || (negate >> n & 1)) {
                if (!is_float && name != "v_cndmask_b32")
                    give_up(std::string(name) + " with a modifier");
                auto value = f(x[n]);
                if (absolute >> n & 1)
                    value = "abs(" + value + ")";
                if (negate >> n & 1)
                    value = "-" + value;
                x[n] = u(value);
            }
        }
        std::string value;
        const auto fx = [&](int n) { return f(x[n]); };
        if (compare) {
            emit_compare(name, destination & 0x7f, x[0], x[1]);
            return;
        }
        if (one) {
            // through the same code as the short encoding, with the modifiers applied after
            const auto before = body_.size();
            emit_vop1(name, destination, x[0]);
            if (modifier || clamp)
                finish_float(before, destination, modifier, clamp, is_half);
            return;
        }
        if (name == "v_cndmask_b32")
            value = "(" + x[2] + " != 0u ? " + x[1] + " : " + x[0] + ")";
        else if (carries) {
            carry(name, destination, (ins.w0 >> 8) & 0x7f, x[0], x[1], carry_in ? x[2] : "0u");
            return;
        } else if (two && ins.op >= 0x100 && ins.op < 0x140)
            value = binary(name, x[0], x[1], v(destination));
        else if (name == "v_mad_f32" || name == "v_fma_f32")
            value = u(fx(0) + " * " + fx(1) + " + " + fx(2));
        else if (name == "v_mad_legacy_f32")
            value = u("mul_legacy(" + fx(0) + ", " + fx(1) + ") + " + fx(2));
        else if (name == "v_fma_f16")
            value = "f32tof16(f16tof32(" + x[0] + ") * f16tof32(" + x[1] + ") + f16tof32(" + x[2] +
                    "))";
        else if (name == "v_min3_f32")
            value = u("min(" + fx(0) + ", min(" + fx(1) + ", " + fx(2) + "))");
        else if (name == "v_max3_f32")
            value = u("max(" + fx(0) + ", max(" + fx(1) + ", " + fx(2) + "))");
        else if (name == "v_med3_f32")
            value = u("clamp(" + fx(0) + ", min(" + fx(1) + ", " + fx(2) + "), max(" + fx(1) +
                      ", " + fx(2) + "))");
        else if (name == "v_min3_f16")
            value = "f32tof16(min(f16tof32(" + x[0] + "), min(f16tof32(" + x[1] + "), f16tof32(" +
                    x[2] + "))))";
        else if (name == "v_max3_f16")
            value = "f32tof16(max(f16tof32(" + x[0] + "), max(f16tof32(" + x[1] + "), f16tof32(" +
                    x[2] + "))))";
        else if (name == "v_med3_f16")
            value = "f32tof16(clamp(f16tof32(" + x[0] + "), min(f16tof32(" + x[1] + "), f16tof32(" +
                    x[2] + ")), max(f16tof32(" + x[1] + "), f16tof32(" + x[2] + "))))";
        else if (name == "v_min3_i32")
            value = "(uint)min((int)" + x[0] + ", min((int)" + x[1] + ", (int)" + x[2] + "))";
        else if (name == "v_max3_i32")
            value = "(uint)max((int)" + x[0] + ", max((int)" + x[1] + ", (int)" + x[2] + "))";
        else if (name == "v_med3_i32")
            value = "(uint)clamp((int)" + x[0] + ", min((int)" + x[1] + ", (int)" + x[2] +
                    "), max((int)" + x[1] + ", (int)" + x[2] + "))";
        else if (name == "v_min3_u32")
            value = "min(" + x[0] + ", min(" + x[1] + ", " + x[2] + "))";
        else if (name == "v_max3_u32")
            value = "max(" + x[0] + ", max(" + x[1] + ", " + x[2] + "))";
        else if (name == "v_med3_u32")
            value = "clamp(" + x[0] + ", min(" + x[1] + ", " + x[2] + "), max(" + x[1] + ", " +
                    x[2] + "))";
        else if (name == "v_bfe_u32")
            value = "((" + x[0] + " >> (" + x[1] + " & 31u)) & ((" + x[2] +
                    " & 31u) == 0u ? 0u : (0xffffffffu >> (32u - (" + x[2] + " & 31u)))))";
        else if (name == "v_bfe_i32")
            value = "((" + x[2] + " & 31u) == 0u ? 0u : (uint)(((int)(" + x[0] + " << (32u - (" +
                    x[2] + " & 31u) - (" + x[1] + " & 31u)))) >> (32u - (" + x[2] + " & 31u))))";
        else if (name == "v_bfi_b32")
            value = "((" + x[0] + " & " + x[1] + ") | (~" + x[0] + " & " + x[2] + "))";
        else if (name == "v_cvt_pk_u8_f32")
            // the float as a byte, into the byte of the third source the second names
            value = "((" + x[2] + " & ~(0xffu << ((" + x[1] +
                    " & 3u) * 8u))) | ((uint)round(clamp(" + fx(0) + ", 0.0, 255.0)) << ((" + x[1] +
                    " & 3u) * 8u)))";
        else if (name == "v_mad_i32_i24")
            value = "(uint)((((int)(" + x[0] + " << 8)) >> 8) * (((int)(" + x[1] +
                    " << 8)) >> 8)) + " + x[2];
        else if (name == "v_mad_u32_u24")
            value = "(" + x[0] + " & 0xffffffu) * (" + x[1] + " & 0xffffffu) + " + x[2];
        else if (name == "v_mul_lo_u32" || name == "v_mul_lo_i32")
            value = x[0] + " * " + x[1];
        else if (name == "v_mul_hi_u32")
            value = "mul_hi(" + x[0] + ", " + x[1] + ")";
        else if (name == "v_mul_hi_i32")
            value = "mul_hi_signed(" + x[0] + ", " + x[1] + ")";
        else if (name == "v_lshl_add_u32")
            value = "(" + x[0] + " << (" + x[1] + " & 31u)) + " + x[2];
        else if (name == "v_add_lshl_u32")
            value = "(" + x[0] + " + " + x[1] + ") << (" + x[2] + " & 31u)";
        else if (name == "v_add3_u32")
            value = x[0] + " + " + x[1] + " + " + x[2];
        else if (name == "v_lshl_or_b32")
            value = "(" + x[0] + " << (" + x[1] + " & 31u)) | " + x[2];
        else if (name == "v_and_or_b32")
            value = "(" + x[0] + " & " + x[1] + ") | " + x[2];
        else if (name == "v_or3_b32")
            value = x[0] + " | " + x[1] + " | " + x[2];
        else if (name == "v_xor3_b32")
            value = x[0] + " ^ " + x[1] + " ^ " + x[2];
        else if (name == "v_xad_u32")
            value = "(" + x[0] + " ^ " + x[1] + ") + " + x[2];
        else if (name == "v_cubeid_f32")
            value = u("cube_id(" + fx(0) + ", " + fx(1) + ", " + fx(2) + ")");
        else if (name == "v_cubesc_f32")
            value = u("cube_sc(" + fx(0) + ", " + fx(1) + ", " + fx(2) + ")");
        else if (name == "v_cubetc_f32")
            value = u("cube_tc(" + fx(0) + ", " + fx(1) + ", " + fx(2) + ")");
        else if (name == "v_cubema_f32")
            value = u("cube_ma(" + fx(0) + ", " + fx(1) + ", " + fx(2) + ")");
        else if (name == "v_ldexp_f32")
            value = u("ldexp(" + fx(0) + ", (float)(int)" + x[1] + ")");
        else if (name == "v_perm_b32")
            value = "perm_bytes(" + x[0] + ", " + x[1] + ", " + x[2] + ")";
        else if (starts(name, "v_mbcnt_") && stage_ == Stage::mesh) {
            // a mesh shader's lanes are the group's threads in order: the lanes of the mask
            // in the half the instruction counts that come before this one
            const auto lane = "(xv_flat & " + std::to_string(mesh_wave_ - 1) + "u)";
            const auto below =
                name == "v_mbcnt_lo_u32_b32"
                    ? "(" + lane + " >= 32u ? 0xffffffffu : (1u << " + lane + ") - 1u)"
                    : "(" + lane + " <= 32u ? 0u : (1u << (" + lane + " - 32u)) - 1u)";
            value = "(" + x[1] + " + countbits(" + x[0] + " & " + below + "))";
        } else if (starts(name, "v_mbcnt_"))
            value = x[1]; // no lane of the wave comes before this one
        else if (name == "v_readlane_b32") {
            set_s(destination & 0x7f, x[0]);
            return;
        } else if (name == "v_alignbit_b32" || name == "v_alignbyte_b32") {
            // the first source over the second as one 64-bit value, moved right
            const auto count =
                name == "v_alignbit_b32" ? "(" + x[2] + " & 31u)" : "((" + x[2] + " & 3u) * 8u)";
            value = "(" + count + " == 0u ? " + x[1] + " : (" + x[1] + " >> " + count + ") | (" +
                    x[0] + " << (32u - " + count + ")))";
        } else if (name == "v_lerp_u8")
            value = "lerp_bytes(" + x[0] + ", " + x[1] + ", " + x[2] + ")";
        else if (name == "v_sad_u8")
            value = "sad_bytes(" + x[0] + ", " + x[1] + ", false) + " + x[2];
        else if (name == "v_msad_u8")
            value = "sad_bytes(" + x[0] + ", " + x[1] + ", true) + " + x[2];
        else if (name == "v_sad_hi_u8")
            value = "(sad_bytes(" + x[0] + ", " + x[1] + ", false) << 16) + " + x[2];
        else if (name == "v_sad_u16")
            value = "(uint)abs((int)(" + x[0] + " & 0xffffu) - (int)(" + x[1] +
                    " & 0xffffu)) + (uint)abs((int)(" + x[0] + " >> 16) - (int)(" + x[1] +
                    " >> 16)) + " + x[2];
        else if (name == "v_sad_u32")
            value = "(" + x[0] + " > " + x[1] + " ? " + x[0] + " - " + x[1] + " : " + x[1] + " - " +
                    x[0] + ") + " + x[2];
        else if (name == "v_mad_u16" || name == "v_mad_i16")
            value = "((" + x[0] + " * " + x[1] + " + " + x[2] + ") & 0xffffu)";
        else if (name == "v_mad_u32_u16")
            value = "(" + x[0] + " & 0xffffu) * (" + x[1] + " & 0xffffu) + " + x[2];
        else if (name == "v_mad_i32_i16")
            value = "(uint)((((int)(" + x[0] + " << 16)) >> 16) * (((int)(" + x[1] +
                    " << 16)) >> 16)) + " + x[2];
        else if (ends(name, "3_i16") || ends(name, "3_u16")) {
            // v_min3, v_max3 and v_med3 of halves
            const bool is_signed = ends(name, "_i16");
            std::string y[3];
            for (int n = 0; n < 3; ++n)
                y[n] = is_signed ? "(((int)(" + x[n] + " << 16)) >> 16)"
                                 : "(int)(" + x[n] + " & 0xffffu)";
            const auto how = std::string(name.substr(2, 3));
            if (how == "med")
                value = "clamp(" + y[0] + ", min(" + y[1] + ", " + y[2] + "), max(" + y[1] + ", " +
                        y[2] + "))";
            else
                value = how + "(" + y[0] + ", " + how + "(" + y[1] + ", " + y[2] + "))";
            value = "((uint)" + value + " & 0xffffu)";
        } else if (two)
            value = binary(name, x[0], x[1], v(destination));
        else
            give_up(std::string(name));
        if (clamp && !is_float) {
            if (name == "v_add_nc_u32")
                value = "(" + x[0] + " + " + x[1] + " < " + x[0] + " ? 0xffffffffu : " + x[0] +
                        " + " + x[1] + ")";
            else if (name == "v_sub_nc_u32")
                value = "(" + x[1] + " > " + x[0] + " ? 0u : " + x[0] + " - " + x[1] + ")";
            else
                give_up(std::string(name) + " that saturates");
        }
        const auto before = body_.size();
        set_v(destination, value);
        if ((modifier || clamp) && is_float)
            finish_float(before, destination, modifier, clamp, is_half);
    }
    // the output modifier and the clamp of an instruction just emitted
    void finish_float(std::size_t, std::uint32_t destination, std::uint32_t modifier,
                      std::uint32_t clamp, bool is_half) {
        const auto reg = v(destination);
        std::string value = is_half ? "f16tof32(" + reg + ")" : f(reg);
        if (modifier == 1)
            value = "(" + value + " * 2.0)";
        if (modifier == 2)
            value = "(" + value + " * 4.0)";
        if (modifier == 3)
            value = "(" + value + " * 0.5)";
        if (clamp)
            value = "saturate(" + value + ")";
        set_v(destination, is_half ? "f32tof16(" + value + ")" : u(value));
    }
    void emit_vop3p(const Ins &ins) {
        // two 16-bit halves at once; each source says which of its halves each half takes
        const auto destination = ins.w0 & 0xff;
        const auto select_low = (ins.w0 >> 11) & 7,
                   select_high = ((ins.w1 >> 27) & 3) | ((ins.w0 >> 14) & 1) << 2;
        const auto negate_low = ins.w1 >> 29, negate_high = (ins.w0 >> 8) & 7;
        const std::uint32_t codes[3] = {ins.w1 & 0x1ff, (ins.w1 >> 9) & 0x1ff,
                                        (ins.w1 >> 18) & 0x1ff};
        const auto name = ins.name;
        const bool three = name == "v_pk_fma_f16" || name == "v_pk_mad_i16" ||
                           name == "v_pk_mad_u16" || starts(name, "v_fma_mix");
        std::string x[3];
        for (int n = 0; n < (three ? 3 : 2); ++n)
            x[n] = operand(ins, codes[n]);
        const auto half = [&](int n, bool high) {
            const auto select = (high ? select_high : select_low) >> n & 1;
            auto value = "f16tof32(" + x[n] + (select ? " >> 16" : "") + ")";
            if ((high ? negate_high : negate_low) >> n & 1)
                value = "-" + value;
            return value;
        };
        if (starts(name, "v_fma_mix")) {
            // a fused multiply and add of floats, each a half (as a float) or a whole one
            const auto mixed = [&](int n) {
                const bool whole = !(select_high >> n & 1);
                auto value = whole
                                 ? f(x[n])
                                 : "f16tof32(" + x[n] + (select_low >> n & 1 ? " >> 16" : "") + ")";
                if (negate_high >> n & 1)
                    value = "abs(" + value + ")";
                if (negate_low >> n & 1)
                    value = "-" + value;
                return value;
            };
            const auto result = mixed(0) + " * " + mixed(1) + " + " + mixed(2);
            if (name == "v_fma_mix_f32")
                set_v(destination, u(result));
            else if (name == "v_fma_mixlo_f16")
                set_v(destination,
                      "(" + v(destination) + " & 0xffff0000u) | f32tof16(" + result + ")");
            else
                set_v(destination,
                      "(" + v(destination) + " & 0xffffu) | (f32tof16(" + result + ") << 16)");
            return;
        }
        if (ends(name, "16") && !ends(name, "_f16")) {
            // integers: each half of the result from the halves the sources say
            const bool is_signed = ends(name, "_i16");
            const auto part = [&](int n, bool high) {
                const auto select = (high ? select_high : select_low) >> n & 1;
                return is_signed ? "(((int)(" + x[n] + (select ? "" : " << 16") + ")) >> 16)"
                                 : "(int)((" + x[n] + (select ? " >> 16" : "") + ") & 0xffffu)";
            };
            const auto one = [&](bool high) -> std::string {
                const auto a = part(0, high), b = part(1, high);
                const auto how = name.substr(5, name.size() - 9);
                if (how == "add")
                    return a + " + " + b;
                if (how == "sub")
                    return a + " - " + b;
                if (how == "mul_lo")
                    return a + " * " + b;
                if (how == "min" || how == "max")
                    return std::string(how) + "(" + a + ", " + b + ")";
                if (how == "mad")
                    return a + " * " + b + " + " + part(2, high);
                if (how == "lshlrev")
                    return b + " << (" + a + " & 15)";
                if (how == "lshrrev" || how == "ashrrev")
                    return b + " >> (" + a + " & 15)";
                give_up(std::string(name));
            };
            if (ins.w0 >> 15 & 1)
                give_up(std::string(name) + " that saturates");
            set_v(destination,
                  "(((uint)(" + one(false) + ") & 0xffffu) | ((uint)(" + one(true) + ") << 16))");
            return;
        }
        const auto each = [&](bool high) -> std::string {
            if (name == "v_pk_mul_f16")
                return half(0, high) + " * " + half(1, high);
            if (name == "v_pk_add_f16")
                return half(0, high) + " + " + half(1, high);
            if (name == "v_pk_min_f16")
                return "min(" + half(0, high) + ", " + half(1, high) + ")";
            if (name == "v_pk_max_f16")
                return "max(" + half(0, high) + ", " + half(1, high) + ")";
            if (name == "v_pk_fma_f16")
                return half(0, high) + " * " + half(1, high) + " + " + half(2, high);
            give_up(std::string(name));
        };
        set_v(destination,
              "(f32tof16(" + each(false) + ") | (f32tof16(" + each(true) + ") << 16))");
    }

    void emit_image(const Ins &ins) {
        const auto name = ins.name;
        const auto dimension = (ins.w0 >> 3) & 7, mask = (ins.w0 >> 8) & 0xf;
        const bool half_address = ins.w1 >> 30 & 1, half_data = ins.w1 >> 31 & 1;
        const auto data = (ins.w1 >> 8) & 0xff;
        const auto texture = resource_of(((ins.w1 >> 16) & 0x1f) * 4);
        const std::string guard = exec_changes_ ? "if " + active() + " " : "";
        // the address: registers in a row from the first, or each one named
        std::vector<std::string> address;
        const auto more = (ins.w0 >> 1) & 3;
        address.push_back(v(ins.w1 & 0xff));
        if (more)
            for (std::uint32_t n = 0; n < more * 4; ++n)
                address.push_back(v((ins.extra[n / 4] >> (8 * (n % 4))) & 0xff));
        else
            for (std::uint32_t n = 1; n < 12; ++n)
                address.push_back("v" + std::to_string(((ins.w1 & 0xff) + n) & 0xff));
        std::size_t next = 0;
        const auto take = [&] {
            if (!more && next)
                used_v_.insert(((ins.w1 & 0xff) + next) & 0xff);
            return address.at(next++);
        };
        if (half_address) { // 16 bits each, two to a register
            std::vector<std::string> halves;
            for (std::size_t n = 0; n < 6; ++n) {
                const auto reg = n < address.size() ? address[n] : std::string("0u");
                if (!more && n)
                    used_v_.insert(((ins.w1 & 0xff) + n) & 0xff);
                halves.push_back("(" + reg + " & 0xffffu)");
                halves.push_back("(" + reg + " >> 16)");
            }
            address = halves;
        }
        static constexpr int coordinates[] = {1, 2, 3, 3, 2, 3, 3, 4};
        static constexpr const char *types[] = {"Texture1D",   "Texture2D",       "Texture3D",
                                                "TextureCube", "Texture1DArray",  "Texture2DArray",
                                                "Texture2DMS", "Texture2DMSArray"};
        const int count = coordinates[dimension];
        const bool load = name == "image_load" || name == "image_load_mip" ||
                          name == "image_store" || name == "image_store_mip" ||
                          starts(name, "image_atomic") || name == "image_get_resinfo";
        std::string result; // a float4, or for a load whatever the texture has
        if (name == "image_get_resinfo") {
            // the size of a level, the layers or the depth, and how many levels there are
            const bool readable = texture.type != 1, multisampled = dimension >= 6;
            const auto object_name =
                object(texture, std::string(readable ? "" : "RW") + types[dimension] + "<float4>");
            static constexpr const char *sizes[] = {"x",    "x, y",    "x, y, z", "x, y",
                                                    "x, z", "x, y, z", "x, y",    "x, y, z"};
            const auto id = std::to_string(temporaries_++);
            line("uint x" + id + "x = 1u, x" + id + "y = 1u, x" + id + "z = 1u, x" + id +
                 "w = 1u;");
            std::string arguments;
            for (const char *size = sizes[dimension]; *size; ++size)
                arguments +=
                    *size == ',' || *size == ' ' ? std::string(1, *size) : "x" + id + *size;
            if (multisampled)
                arguments += ", x" + id + "w";
            else if (readable)
                arguments = "(uint)" + take() + ", " + arguments + ", x" + id + "w";
            line(object_name + ".GetDimensions(" + arguments + ");");
            if (dimension == 3)
                line("x" + id + "z = 6u;");
            std::uint32_t reg = data;
            for (int c = 0; c < 4; ++c)
                if (mask >> c & 1)
                    set_v(reg++, "x" + id + "xyzw"[c]);
            return;
        }
        if (load) {
            if (dimension == 3)
                give_up("a cube read by texel");
            std::string where;
            const auto integer = [&](const std::string &reg) {
                return half_address ? "(int)" + reg : "(int)" + reg;
            };
            std::vector<std::string> xs;
            for (int n = 0; n < count; ++n)
                xs.push_back(integer(take()));
            const bool multisampled = dimension >= 6;
            const int places = multisampled ? count - 1 : count;
            where = "int" + std::to_string(places) + "(";
            for (int n = 0; n < places; ++n)
                where += (n ? ", " : "") + xs[n];
            where += ")";
            if (name == "image_load" || name == "image_load_mip") {
                const auto level = name == "image_load_mip" ? integer(take()) : std::string("0");
                if (texture.type == 1)
                    result = object(texture, std::string("RW") + types[dimension] + "<float4>") +
                             "[" + where + "]";
                else if (multisampled)
                    result = object(texture, std::string(types[dimension]) + "<float4>") +
                             ".Load(" + where + ", " + xs[count - 1] + ")";
                else
                    result = object(texture, std::string(types[dimension]) + "<float4>") +
                             ".Load(int" + std::to_string(places + 1) + "(" + where + ", " + level +
                             "))";
            } else if (name == "image_store" || name == "image_store_mip") {
                written(texture);
                const auto target =
                    object(texture, std::string("RW") + types[dimension] + "<float4>");
                std::string value = "float4(";
                std::uint32_t reg = data;
                for (int c = 0; c < 4; ++c)
                    value +=
                        std::string(c ? ", " : "") +
                        (mask >> c & 1 ? (half_data ? "f16tof32(" + v(reg++) + ")" : f(v(reg++)))
                                       : std::string("0.0"));
                value += ")";
                line(guard + target + "[" + where + "] = " + value + ";");
                return;
            } else {
                // an atomic operation on a texel, which leaves what was there before
                written(texture);
                const auto target =
                    object(texture, std::string("RW") + types[dimension] + "<uint>") + "[" + where +
                    "]";
                const auto id = "x" + std::to_string(temporaries_++);
                line("uint " + id + " = 0u;");
                line(guard + atomic(name.substr(13), target, v(data), v(data + 1), id));
                set_v(data, id);
                return;
            }
        } else {
            const auto sampler_register = ((ins.w1 >> 21) & 0x1f) * 4;
            auto state = sampler_of(sampler_register);
            if (state.dynamic >= 0)
                state.type = 3;
            if (state.type != 3)
                give_up("a sampler that is not one");
            if (texture.type != 0)
                give_up("a sampled texture that is not a shader resource");
            // image_sample[_c][_d|_l|_b|_lz|_cl][_o], image_gather4[_c][_l|_lz][_o]
            const auto parts = name.substr(starts(name, "image_gather4") ? 13 : 12);
            const auto has = [&](std::string_view part) {
                const auto at = parts.find(part);
                return at != std::string_view::npos &&
                       (at + part.size() == parts.size() || parts[at + part.size()] == '_');
            };
            const bool gather = starts(name, "image_gather4"), compare = has("_c"),
                       offset = has("_o"), gradients = has("_d") || has("_cd"), level = has("_l"),
                       bias = has("_b"), zero = has("_lz"), clamped = has("_cl");
            if (has("_g16") || (gather && clamped))
                give_up(std::string(name));
            std::string offset_value, bias_value, reference, ddx_value, ddy_value, level_value;
            const auto real = [&](const std::string &reg) {
                return half_address ? "f16tof32(" + reg + ")" : f(reg);
            };
            if (offset) {
                const auto packed = take();
                const int places = dimension == 2 ? 3 : 2;
                offset_value = ", int" + std::to_string(places) + "(";
                for (int n = 0; n < places; ++n)
                    offset_value += std::string(n ? ", " : "") + "(((int)(" + packed + " << " +
                                    std::to_string(26 - 8 * n) + ")) >> 26)";
                offset_value += ")";
            }
            if (bias)
                bias_value = real(take());
            if (compare)
                reference = real(take());
            const int gradient_places = dimension == 2 || dimension == 3   ? 3
                                        : dimension == 0 || dimension == 4 ? 1
                                                                           : 2;
            if (gradients) {
                std::string x = "float" + std::to_string(gradient_places) + "(", y = x;
                for (int n = 0; n < gradient_places; ++n)
                    x += (n ? ", " : "") + real(take());
                for (int n = 0; n < gradient_places; ++n)
                    y += (n ? ", " : "") + real(take());
                ddx_value = x + ")", ddy_value = y + ")";
            }
            if (dimension == 3 && gradients)
                give_up("a cube sampled with gradients");
            std::string where = "float" + std::to_string(count) + "(";
            for (int n = 0; n < count; ++n)
                where += (n ? ", " : "") + real(take());
            where += ")";
            // the GPU takes a cube's face and the place on it; the host a direction
            if (dimension == 3)
                where = "cube_direction(" + where + ")";
            if (level)
                level_value = real(take());
            // The least level a sample may come from. The host takes it after an offset,
            // which a cube has none of.
            std::string clamp_value;
            if (clamped) {
                static constexpr const char *none[] = {", 0", ", int2(0, 0)", ", int3(0, 0, 0)",
                                                       "",    ", 0",          ", int2(0, 0)"};
                if (dimension >= 6)
                    give_up(std::string(name));
                clamp_value = (offset_value.empty() ? std::string(none[dimension]) : "") + ", " +
                              real(take());
            }
            const auto texture_name = object(texture, std::string(types[dimension]) + "<float4>");
            const auto sampler = object(state, compare ? "SamplerComparisonState" : "SamplerState");
            if (name == "image_get_lod") {
                if (stage_ != Stage::pixel)
                    give_up("image_get_lod outside a pixel shader");
                // both levels are found before either is written: they may go to the
                // registers of the address
                const auto levels = "x" + std::to_string(temporaries_++);
                line("float2 " + levels + " = float2(" + texture_name + ".CalculateLevelOfDetail(" +
                     sampler + ", " + where + "), " + texture_name +
                     ".CalculateLevelOfDetailUnclamped(" + sampler + ", " + where + "));");
                std::uint32_t reg = data;
                if (mask & 1)
                    set_v(reg++, u(levels + ".x"));
                if (mask & 2)
                    set_v(reg++, u(levels + ".y"));
                return;
            }
            const auto &object = texture_name;
            if (gather) {
                const int component = std::countr_zero(mask);
                static constexpr const char *methods[] = {"GatherRed", "GatherGreen", "GatherBlue",
                                                          "GatherAlpha"};
                result = object + "." + (compare ? "GatherCmp" : methods[component & 3]) + "(" +
                         sampler + ", " + where + (compare ? ", " + reference : "") + offset_value +
                         ")";
                // the four texels, in the registers one after the other
                const auto sample = "x" + std::to_string(temporaries_++);
                line("float4 " + sample + " = " + result + ";");
                for (int c = 0; c < 4; ++c)
                    set_v(data + c, u(sample + "." + "xyzw"[c]));
                return;
            }
            const bool implicit = stage_ == Stage::pixel && open_loops_.empty();
            if (compare) {
                if (bias || gradients)
                    give_up(std::string(name));
                if (!zero && !level && implicit)
                    result = "(" + object + ".SampleCmp(" + sampler + ", " + where + ", " +
                             reference + offset_value + clamp_value + ")).xxxx";
                else
                    result = "(" + object + ".SampleCmpLevelZero(" + sampler + ", " + where + ", " +
                             reference + offset_value + ")).xxxx";
            } else if (gradients) {
                result = object + ".SampleGrad(" + sampler + ", " + where + ", " + ddx_value +
                         ", " + ddy_value + offset_value + clamp_value + ")";
            } else if (zero || level || stage_ != Stage::pixel || !open_loops_.empty()) {
                result = object + ".SampleLevel(" + sampler + ", " + where + ", " +
                         (level ? level_value : std::string("0.0")) + offset_value + ")";
            } else if (bias) {
                result = object + ".SampleBias(" + sampler + ", " + where + ", " + bias_value +
                         offset_value + clamp_value + ")";
            } else {
                result =
                    object + ".Sample(" + sampler + ", " + where + offset_value + clamp_value + ")";
            }
        }
        const auto sample = "x" + std::to_string(temporaries_++);
        line("float4 " + sample + " = " + result + ";");
        std::uint32_t reg = data;
        for (int c = 0; c < 4; ++c)
            if (mask >> c & 1) {
                const auto channel = sample + "." + "xyzw"[c];
                set_v(reg++, half_data ? "f32tof16(" + channel + ")" : u(channel));
            }
    }
    // An atomic operation named by the end of its instruction's name, as a statement:
    // `target` takes `value`, and `before` what was there.
    std::string atomic(std::string_view how, const std::string &target, const std::string &value,
                       const std::string &compare, const std::string &before) {
        const auto call = [&](const char *function, const std::string &with) {
            return std::string(function) + "(" + target + ", " + with + ", " + before + ");";
        };
        if (how == "add")
            return call("InterlockedAdd", value);
        if (how == "sub")
            return call("InterlockedAdd", "(uint)(-(int)" + value + ")");
        if (how == "inc")
            return call("InterlockedAdd", "1u");
        if (how == "dec")
            return call("InterlockedAdd", "0xffffffffu");
        if (how == "umin")
            return call("InterlockedMin", value);
        if (how == "umax")
            return call("InterlockedMax", value);
        if (how == "and")
            return call("InterlockedAnd", value);
        if (how == "or")
            return call("InterlockedOr", value);
        if (how == "xor")
            return call("InterlockedXor", value);
        if (how == "swap")
            return call("InterlockedExchange", value);
        if (how == "cmpswap")
            return call("InterlockedCompareExchange", compare + ", " + value);
        // no host intrinsic: compare-exchange loop, X is the value read
        const std::string fx = "asfloat(X)", fv = "asfloat(" + value + ")";
        std::string next;
        if (how == "rsub")
            next = value + " - X";
        else if (how == "wrapinc")
            next = "(X >= " + value + " ? 0u : X + 1u)";
        else if (how == "wrapdec")
            next = "(X == 0u || X > " + value + " ? " + value + " : X - 1u)";
        else if (how == "smin")
            next = "(uint)min((int)X, (int)" + value + ")";
        else if (how == "smax")
            next = "(uint)max((int)X, (int)" + value + ")";
        else if (how == "mskor")
            next = "((X & ~" + value + ") | " + compare + ")";
        else if (how == "wrap")
            next = "(X >= " + value + " ? X - " + value + " : X + " + compare + ")";
        else if (how == "fmin")
            next = "asuint(min(" + fx + ", " + fv + "))";
        else if (how == "fmax")
            next = "asuint(max(" + fx + ", " + fv + "))";
        else if (how == "fadd")
            next = "asuint(" + fx + " + " + fv + ")";
        else if (how == "fcmpswap")
            next = "(" + fx + " == asfloat(" + compare + ") ? " + value + " : X)";
        else
            give_up("an atomic " + std::string(how));
        return "{ uint X; do { X = " + target + "; InterlockedCompareExchange(" + target + ", X, " +
               next + ", " + before + "); } while (" + before + " != X); }";
    }
    // The components of a typed buffer access's format, as the Series GPU numbers its buffer
    // formats (the number in an MTBUF instruction, which buffer_formats makes the older
    // ones into): bits of each from the lowest, and how they are read - u unorm, s snorm,
    // U uscaled, S sscaled, i uint, I sint, f float. Nothing for 0 and the packed floats.
    struct TypedFormat {
        std::uint32_t bits[4] = {};
        std::uint32_t count = 0;
        char kind = 0;
    };
    static std::optional<TypedFormat> typed_format(std::uint32_t format) {
        struct Group {
            std::uint32_t first, last;
            std::uint32_t bits[4], count;
            const char *kinds;
        };
        static constexpr Group groups[] = {
            {1, 6, {8}, 1, "usUSiI"},
            {7, 13, {16}, 1, "usUSiIf"},
            {14, 19, {8, 8}, 2, "usUSiI"},
            {20, 22, {32}, 1, "iIf"},
            {23, 29, {16, 16}, 2, "usUSiIf"},
            {30, 35, {11, 11, 10}, 3, "usUSiI"},
            {37, 42, {10, 11, 11}, 3, "usUSiI"},
            {44, 49, {2, 10, 10, 10}, 4, "usUSiI"},
            {50, 55, {10, 10, 10, 2}, 4, "usUSiI"},
            {56, 61, {8, 8, 8, 8}, 4, "usUSiI"},
            {62, 64, {32, 32}, 2, "iIf"},
            {65, 71, {16, 16, 16, 16}, 4, "usUSiIf"},
            {72, 74, {32, 32, 32}, 3, "iIf"},
            {75, 77, {32, 32, 32, 32}, 4, "iIf"},
        };
        for (const auto &group : groups)
            if (format >= group.first && format <= group.last) {
                TypedFormat typed;
                std::copy(std::begin(group.bits), std::end(group.bits), typed.bits);
                typed.count = group.count;
                typed.kind = group.kinds[format - group.first];
                return typed;
            }
        return std::nullopt;
    }
    // tbuffer_load_format_* and tbuffer_store_format_*: element `index` of a buffer, read and
    // converted by the format the instruction names, whatever the descriptor's says
    void emit_typed_buffer(const Ins &ins, const Resource &resource, const std::string &index,
                           const std::string &extra, std::uint32_t fixed, std::uint32_t data) {
        const auto name = ins.name;
        const bool store = starts(name, "tbuffer_store_format_");
        if ((!store && !starts(name, "tbuffer_load_format_")) || name.find("d16") != name.npos)
            give_up(std::string(name));
        const auto components = static_cast<std::uint32_t>(name.size() - (store ? 21 : 20));
        const auto typed = typed_format((ins.w0 >> 19) & 0x7f);
        if (!typed || components < 1 || components > 4)
            give_up(std::string(name) + " of buffer format " +
                    std::to_string((ins.w0 >> 19) & 0x7f));
        std::uint32_t total = 0;
        for (std::uint32_t c = 0; c < typed->count; ++c)
            total += typed->bits[c];
        const bool whole = typed->kind == 'i' || typed->kind == 'I' || typed->kind == 'f';
        if (store) {
            // only formats of whole words, which are written as they are
            if (!whole || typed->bits[0] != 32)
                give_up(std::string(name) + " of a format of parts of words");
            for (std::uint32_t c = 0; c < std::min(components, typed->count); ++c)
                write_word(resource, index, extra, fixed, 4 * c, v(data + c));
            return;
        }
        // the words the element is in; one smaller than a word is where in it the offset
        // says
        std::vector<std::string> word;
        if (total < 32) {
            const auto id = "x" + std::to_string(temporaries_++);
            line("uint " + id + " = " +
                 read_word(resource, index, extra, fixed == ~0u ? fixed : fixed & ~3u, 0) +
                 " >> ((" + extra + " & 3u) * 8u);");
            word.push_back(id);
        } else {
            for (std::uint32_t n = 0; n < total / 32; ++n) {
                const auto id = "x" + std::to_string(temporaries_++);
                line("uint " + id + " = " + read_word(resource, index, extra, fixed, 4 * n) + ";");
                word.push_back(id);
            }
        }
        const bool integer = typed->kind == 'i' || typed->kind == 'I';
        std::uint32_t at = 0;
        for (std::uint32_t c = 0; c < components; ++c) {
            if (c >= typed->count) {
                set_v(data + c, c == 3 ? (integer ? "1u" : "0x3f800000u") : "0u");
                continue;
            }
            const auto bits = typed->bits[c];
            const auto source = word[at / 32];
            const auto mask = bits == 32 ? std::string("0xffffffffu") : hex((1u << bits) - 1);
            const auto raw =
                "((" + source + " >> " + std::to_string(at % 32) + "u) & " + mask + ")";
            const auto extended = bits == 32
                                      ? "(int)" + raw
                                      : "((int)(" + raw + " << " + std::to_string(32 - bits) +
                                            "u) >> " + std::to_string(32 - bits) + ")";
            const auto top = std::to_string((1u << (bits - 1)) - 1) + ".0";
            std::string value;
            switch (typed->kind) {
            case 'u':
                value = u("(float(" + raw + ") / " + std::to_string((1ull << bits) - 1) + ".0)");
                break;
            case 's':
                value = u("max(float(" + extended + ") / " + top + ", -1.0)");
                break;
            case 'U':
                value = u("float(" + raw + ")");
                break;
            case 'S':
                value = u("float(" + extended + ")");
                break;
            case 'i':
                value = raw;
                break;
            case 'I':
                value = "(uint)" + extended;
                break;
            default:
                value = bits == 16 ? u("f16tof32(" + raw + ")") : raw;
            }
            set_v(data + c, value);
            at += bits;
        }
    }
    void emit_buffer(const Ins &ins) {
        const auto name = ins.name;
        if (name == "buffer_gl0_inv" || name == "buffer_gl1_inv")
            return;
        const auto offset = ins.w0 & 0xfff;
        const bool has_offset = ins.w0 >> 12 & 1, has_index = ins.w0 >> 13 & 1;
        const auto address = ins.w1 & 0xff, data = (ins.w1 >> 8) & 0xff;
        const auto base = ((ins.w1 >> 16) & 0x1f) * 4, scalar_offset = ins.w1 >> 24;
        const auto resource = resource_of(base);
        if (resource.stream) {
            // an input of the vertex, by where it is in the stream's vertices
            const bool raw = starts(name, "buffer_load_dword");
            const bool typed = starts(name, "tbuffer_load_format_");
            if ((!starts(name, "buffer_load_format_") && !raw && !typed) || !has_index ||
                has_offset)
                give_up("a vertex stream read another way than as an input (" + std::string(name) +
                        ")");
            const auto rest = name.substr(17);
            const auto components =
                !raw           ? static_cast<std::uint32_t>(name.size() - (typed ? 20 : 19))
                : rest.empty() ? 1u
                : rest == "x2" ? 2u
                : rest == "x3" ? 3u
                               : 4u;
            const auto &inputs = shader_.inputs;
            for (std::size_t i = 0; i < inputs.size(); ++i) {
                const auto &element = inputs[i];
                // how many bytes each component of the element is, by its format; a read
                // may start at a later component than the first
                const auto format = element.format;
                const std::uint32_t bytes =
                    (format >= 27 && format <= 32) || (format >= 48 && format <= 52) ||
                            (format >= 60 && format <= 64)
                        ? 1
                    : (format >= 9 && format <= 14) || (format >= 33 && format <= 38) ||
                            (format >= 53 && format <= 59)
                        ? 2
                        : 4;
                if (generated(element) || element.stream != resource.reg ||
                    offset < element.offset || offset >= element.offset + bytes * element.width ||
                    (offset - element.offset) % bytes)
                    continue;
                const auto first = (offset - element.offset) / bytes;
                if (first) {
                    if (raw)
                        give_up("a vertex stream read as words from the middle of an input");
                    for (std::uint32_t c = 0; c < components; ++c) {
                        const auto field =
                            "input.f" + std::to_string(i) + "." + "xyzw"[(first + c) & 3];
                        set_v(data + c, first + c >= element.width ? (c == 3 ? "0x3f800000u" : "0u")
                                        : element.type == 3        ? u(field)
                                                                   : "(uint)" + field);
                    }
                    return;
                }
                // the words as they lie in the vertex: of four bytes, the bytes again
                if (raw && element.format >= 27 && element.format <= 32 && components == 1) {
                    const auto field = "input.f" + std::to_string(i);
                    const bool whole = element.format == 30 || element.format == 32;
                    const bool is_signed = element.format == 31 || element.format == 32;
                    const std::string scale = is_signed ? "127.0" : "255.0";
                    const auto bytes = whole ? "uint4(" + field + ")"
                                             : "uint4(int4(round(" + field + " * " + scale + ")))";
                    const auto id = "x" + std::to_string(temporaries_++);
                    line("uint4 " + id + " = " + bytes + " & 255u;");
                    set_v(data, id + ".x | (" + id + ".y << 8) | (" + id + ".z << 16) | (" + id +
                                    ".w << 24)");
                    return;
                }
                if (raw && components != element.width)
                    give_up("a vertex stream read as " + std::to_string(components) +
                            " words where the input layout has format " +
                            std::to_string(element.format));
                for (std::uint32_t c = 0; c < components; ++c) {
                    const auto field = "input.f" + std::to_string(i) +
                                       (element.width > 1 ? std::string(".") + "xyzw"[c] : "");
                    set_v(data + c, c >= element.width  ? (c == 3 ? "0x3f800000u" : "0u")
                                    : element.type == 3 ? u(field)
                                                        : "(uint)" + field);
                }
                return;
            }
            give_up("a vertex stream read where the input layout has nothing");
        }
        const std::string guard = exec_changes_ ? "if " + active() + " " : "";
        std::string index = "0u", extra = hex(offset);
        std::uint32_t fixed = offset;
        if (has_index)
            index = v(address);
        if (has_offset)
            extra = "(" + v(address + (has_index ? 1 : 0)) + " + " + extra + ")", fixed = ~0u;
        if (scalar_offset != 0x80 && scalar_offset != 125)
            extra = "(" + operand(ins, scalar_offset) + " + " + extra + ")", fixed = ~0u;
        if (ins.format == F::mtbuf) {
            emit_typed_buffer(ins, resource, index, extra, fixed, data);
            return;
        }
        if (starts(name, "buffer_load_format_") || starts(name, "buffer_store_format_")) {
            // a buffer of elements with a format: element `index`
            const bool store = starts(name, "buffer_store");
            const auto components = name.size() - (store ? 20 : 19);
            if (resource.root || resource.type >= 2)
                give_up("a buffer with a format that is not a view");
            if (ends(name.substr(0, name.size() - components - 1), "d16"))
                give_up(std::string(name));
            if (store) {
                written(resource);
                std::string value = "float4(";
                for (std::size_t c = 0; c < 4; ++c)
                    value += std::string(c ? ", " : "") +
                             (c < components ? f(v(data + static_cast<std::uint32_t>(c)))
                                             : std::string("0.0"));
                line(guard + object(resource, "RWBuffer<float4>") + "[" + index + "] = " + value +
                     ");");
                return;
            }
            const auto buffer =
                object(resource, resource.type == 1 ? "RWBuffer<float4>" : "Buffer<float4>");
            const auto sample = "x" + std::to_string(temporaries_++);
            line("float4 " + sample + " = " + buffer +
                 (resource.type == 1 ? "[" + index + "]" : ".Load(" + index + ")") + ";");
            for (std::size_t c = 0; c < components; ++c)
                set_v(data + static_cast<std::uint32_t>(c), u(sample + "." + "xyzw"[c]));
            return;
        }
        const auto count = [&](std::string_view prefix) -> std::uint32_t {
            const auto rest = name.substr(prefix.size());
            return rest.empty() ? 1 : rest == "x2" ? 2 : rest == "x3" ? 3 : rest == "x4" ? 4 : 0;
        };
        if (starts(name, "buffer_load_dword") && count("buffer_load_dword")) {
            // all of them read before any is written: the address may be in one of them
            const auto words = count("buffer_load_dword");
            std::vector<std::string> read;
            for (std::uint32_t n = 0; n < words; ++n) {
                read.push_back(words > 1 ? "x" + std::to_string(temporaries_++) : "");
                const auto value = read_word(resource, index, extra, fixed, 4 * n);
                if (words > 1)
                    line("uint " + read.back() + " = " + value + ";");
                else
                    read.back() = value;
            }
            for (std::uint32_t n = 0; n < words; ++n)
                set_v(data + n, read[n]);
            return;
        }
        if (starts(name, "buffer_store_dword") && count("buffer_store_dword")) {
            for (std::uint32_t n = 0, words = count("buffer_store_dword"); n < words; ++n)
                write_word(resource, index, extra, fixed, 4 * n, v(data + n));
            return;
        }
        const bool upper = ends(name, "_d16_hi"), half = upper || ends(name, "_d16");
        const auto plain = name.substr(0, name.size() - (upper ? 7 : half ? 4 : 0));
        const auto shift = "((" + extra + " & 3u) * 8u)";
        if (plain == "buffer_load_ubyte" || plain == "buffer_load_sbyte" ||
            plain == "buffer_load_ushort" || plain == "buffer_load_sshort" ||
            (half && plain == "buffer_load_short")) {
            // part of a word, from where in it the offset says
            const bool byte = ends(plain, "byte"), extend = plain[12] == 's';
            const auto part =
                "((" + read_word(resource, index, extra, fixed == ~0u ? fixed : fixed & ~3u, 0) +
                " >> " + shift + ") & " + (byte ? "0xffu" : "0xffffu") + ")";
            const auto bits = byte ? "24" : "16";
            const auto value =
                extend ? "(uint)(((int)(" + part + " << " + bits + ")) >> " + bits + ")" : part;
            set_v(data, !half   ? value
                        : upper ? "(" + v(data) + " & 0xffffu) | (" + value + " << 16)"
                                : "(" + v(data) + " & 0xffff0000u) | (" + value + " & 0xffffu)");
            return;
        }
        if ((plain == "buffer_store_byte" || plain == "buffer_store_short") && (!half || upper)) {
            // part of a word, whose other parts another thread may be writing
            const auto mask = plain == "buffer_store_byte" ? "0xffu" : "0xffffu";
            const auto clear = "~(" + std::string(mask) + " << " + shift + ")";
            const auto part = "((" + (upper ? "(" + v(data) + " >> 16)" : v(data)) + " & " + mask +
                              ") << " + shift + ")";
            written(resource);
            if (resource.root) {
                const auto buffer = object(resource, "RWByteAddressBuffer");
                const auto at = bytes_of(resource, index) + "(" + extra + " & ~3u)";
                line(guard + "{ uint X; " + buffer + ".InterlockedAnd(" + at + ", " + clear +
                     ", X); " + buffer + ".InterlockedOr(" + at + ", " + part + ", X); }");
            } else {
                const auto word =
                    words(resource) + "[" + index + "].w[" +
                    field("(" + extra + " + 0u)", fixed == ~0u ? fixed : fixed & ~3u, 0) + "]";
                line(guard + "{ InterlockedAnd(" + word + ", " + clear + "); InterlockedOr(" +
                     word + ", " + part + "); }");
            }
            return;
        }
        if (starts(name, "buffer_atomic_") && !ends(name, "_x2")) {
            written(resource);
            const auto at = "(" + extra + " + 0u)";
            const auto id = "x" + std::to_string(temporaries_++);
            line("uint " + id + " = 0u;");
            if (resource.root) {
                const auto how = name.substr(14);
                static constexpr std::pair<const char *, const char *> methods[] = {
                    {"add", "InterlockedAdd"},      {"umin", "InterlockedMin"},
                    {"umax", "InterlockedMax"},     {"and", "InterlockedAnd"},
                    {"or", "InterlockedOr"},        {"xor", "InterlockedXor"},
                    {"swap", "InterlockedExchange"}};
                const char *method = nullptr;
                for (const auto &[suffix, function] : methods)
                    if (how == suffix)
                        method = function;
                if (!method)
                    give_up("an atomic " + std::string(how) + " on a root buffer");
                line(guard + object(resource, "RWByteAddressBuffer") + "." + method + "(" +
                     bytes_of(resource, index) + at + ", " + v(data) + ", " + id + ");");
            } else {
                line(guard +
                     atomic(name.substr(14),
                            words(resource) + "[" + index + "].w[" + field(at, fixed, 0) + "]",
                            v(data), v(data + 1), id));
            }
            set_v(data, id);
            return;
        }
        give_up(std::string(name));
    }

    void emit_export(const Ins &ins) {
        const auto target = (ins.w0 >> 4) & 0x3f, enabled = ins.w0 & 0xf;
        const bool compressed = ins.w0 >> 10 & 1, valid_mask = ins.w0 >> 12 & 1;
        std::string channels[4] = {"0.0", "0.0", "0.0", "0.0"};
        if (compressed) {
            for (int pair = 0; pair < 2; ++pair) {
                if (!(enabled >> (2 * pair) & 3))
                    continue;
                const auto reg = v((ins.w1 >> (8 * pair)) & 0xff);
                channels[2 * pair] = "f16tof32(" + reg + ")";
                channels[2 * pair + 1] = "f16tof32(" + reg + " >> 16)";
            }
        } else {
            for (int n = 0; n < 4; ++n)
                if (enabled >> n & 1)
                    channels[n] = f(v((ins.w1 >> (8 * n)) & 0xff));
        }
        const auto &outputs = shader_.outputs;
        const auto assign = [&](const std::string &field, const std::string &value) {
            // a mesh shader's lane writes its own vertex
            const std::string to = stage_ == Stage::mesh ? "xv_verts[xv_flat].f" : "output.f";
            const std::string guard =
                stage_ == Stage::mesh ? "xv_flat < " + std::to_string(mesh_vertices_) + "u" : "";
            line(std::string(exec_changes_ || !guard.empty()
                                 ? "if (" + (exec_changes_ ? active() : std::string("true")) +
                                       (guard.empty() ? "" : " && " + guard) + ") "
                                 : "") +
                 to + field + " = " + value + ";");
        };
        // a parameter of 16-bit registers: each word holds two halves (see `packed`)
        const auto write_packed = [&](std::uint32_t row, const std::set<std::uint32_t> &own) {
            if (compressed)
                give_up("a compressed export of 16-bit outputs");
            for (std::uint32_t half = 0; half < 8; ++half) {
                const auto [field, component] = packed(outputs, row, own)[half];
                if (field < 0 || !(enabled >> (half / 2) & 1))
                    continue;
                const auto &element = outputs[field];
                const auto bits = "(" + v((ins.w1 >> (8 * (half / 2))) & 0xff) +
                                  (half & 1 ? " >> 16" : "") + " & 0xffffu)";
                assign(std::to_string(field) +
                           (element.width > 1 ? std::string(".") + "xyzw"[component] : ""),
                       element.type == 6   ? "f16tof32(" + bits + ")"
                       : element.type == 5 ? "(float)((int)(" + bits + " << 16) >> 16)"
                                           : "(float)" + bits);
            }
        };
        const auto write = [&](std::size_t field, std::uint32_t from) {
            const auto &element = outputs[field];
            std::string value;
            std::uint32_t places = 0;
            for (std::uint32_t channel = from; channel < 4 && places < element.width;
                 ++channel, ++places)
                value +=
                    (places ? ", " : "") +
                    (element.type == 3 ? channels[channel] : "asuint(" + channels[channel] + ")");
            for (; places < element.width; ++places)
                value += ", 0";
            const std::string type = element.type == 3   ? "float"
                                     : element.type == 2 ? "int"
                                                         : "uint";
            // a mesh shader's lane writes its own vertex
            const std::string to = stage_ == Stage::mesh ? "xv_verts[xv_flat].f" : "output.f";
            const std::string guard =
                stage_ == Stage::mesh ? "xv_flat < " + std::to_string(mesh_vertices_) + "u" : "";
            line(std::string(exec_changes_ || !guard.empty()
                                 ? "if (" + (exec_changes_ ? active() : std::string("true")) +
                                       (guard.empty() ? "" : " && " + guard) + ") "
                                 : "") +
                 to + std::to_string(field) + " = " +
                 (element.width > 1 ? type + std::to_string(element.width) + "(" + value + ")"
                                    : "(" + type + ")(" + value + ")") +
                 ";");
        };
        bool written = false;
        if (stage_ == Stage::pixel && valid_mask && exec_changes_)
            line("if (!" + active() + ") discard;");
        if (stage_ == Stage::mesh && target == 20) {
            // the lane's primitive: three vertex numbers of ten bits; bit 31 culls it
            const auto value = v(ins.w1 & 0xff);
            line("if (" + std::string(exec_changes_ ? active() + " && " : "") + "xv_flat < " +
                 std::to_string(mesh_primitives_) + "u) xv_prims[xv_flat] = (" + value +
                 " & 0x80000000u) != 0u ? uint3(0u, 0u, 0u) : uint3(" + value + " & 0x3ffu, (" +
                 value + " >> 10) & 0x3ffu, (" + value + " >> 20) & 0x3ffu);");
            exported_ = true;
            return;
        }
        if (stage_ == Stage::mesh && target == 12) {
            for (std::size_t i = 0; i < outputs.size(); ++i)
                if (!_stricmp(outputs[i].name.c_str(), "SV_Position"))
                    write(i, 0), written = true;
        } else if (stage_ == Stage::mesh && target >= 32) {
            // parameter n: the row the stage data gives it
            std::set<std::uint32_t> own;
            for (const auto &[parameter, row] : mesh_outputs_)
                own.insert(row);
            for (const auto &[parameter, row] : mesh_outputs_)
                if (parameter == target - 32 && sixteen_at(outputs, row))
                    write_packed(row, own), written = true;
                else if (parameter == target - 32)
                    for (std::size_t i = 0; i < outputs.size(); ++i)
                        if (outputs[i].reg == row &&
                            _stricmp(outputs[i].name.c_str(), "SV_Position"))
                            write(i,
                                  static_cast<std::uint32_t>(std::countr_zero(static_cast<unsigned>(
                                      outputs[i].mask ? outputs[i].mask : 1)))),
                                written = true;
            for (const auto &[parameter, row] : mesh_outputs_)
                if ((parameter & 0x7f) == target - 32 && parameter & 0x80)
                    give_up("a mesh shader's output of its primitives");
            if (!written)
                return;
        } else if (stage_ == Stage::mesh && target == 13) {
            return; // more of the position
        } else if (stage_ == Stage::vertex && target == 12) {
            for (std::size_t i = 0; i < outputs.size(); ++i)
                if (!_stricmp(outputs[i].name.c_str(), "SV_Position"))
                    write(i, 0), written = true;
        } else if (stage_ == Stage::vertex && target >= 32) {
            // parameter n: the nth register that is not the position's, nor packed with a
            // 16-bit one before it
            const auto after = packed_after(outputs, {});
            std::vector<std::uint32_t> registers;
            for (const auto &element : outputs)
                if (_stricmp(element.name.c_str(), "SV_Position") && !after.count(element.reg) &&
                    std::find(registers.begin(), registers.end(), element.reg) == registers.end())
                    registers.push_back(element.reg);
            std::sort(registers.begin(), registers.end());
            if (target - 32 < registers.size() && sixteen_at(outputs, registers[target - 32]))
                write_packed(registers[target - 32], {}), written = true;
            else if (target - 32 < registers.size())
                for (std::size_t i = 0; i < outputs.size(); ++i)
                    if (outputs[i].reg == registers[target - 32] &&
                        _stricmp(outputs[i].name.c_str(), "SV_Position"))
                        write(i, static_cast<std::uint32_t>(std::countr_zero(static_cast<unsigned>(
                                     outputs[i].mask ? outputs[i].mask : 1)))),
                            written = true;
            if (!written)
                return; // a parameter nothing after this stage reads
        } else if (stage_ == Stage::vertex && (target == 13 || target == 20)) {
            return; // more of the position (clip distances, point size) and the primitive's
        } else if (stage_ == Stage::pixel && target < 8) {
            for (std::size_t i = 0; i < outputs.size(); ++i)
                if (!_stricmp(outputs[i].name.c_str(), "SV_Target") && outputs[i].index == target)
                    write(i, 0), written = true;
            if (!written)
                return;
        } else if (stage_ == Stage::pixel && target == 8) {
            for (std::size_t i = 0; i < outputs.size(); ++i)
                if (!_strnicmp(outputs[i].name.c_str(), "SV_Depth", 8))
                    write(i, 0), written = true;
            if (!written)
                return;
        } else if (target == 9) {
            exported_ |= stage_ == Stage::pixel;
            return; // the null target: only its valid mask counts
        }
        if (!written)
            give_up("export to target " + std::to_string(target));
        exported_ = true;
    }

    static std::string field_type(const Element &element) {
        return std::string(element.type == 1   ? "uint"
                           : element.type == 2 ? "int"
                                               : "float") +
               (element.width > 1 ? std::to_string(element.width) : "");
    }
    static bool generated(const Element &element) {
        for (const auto *made : {"SV_VertexID", "SV_InstanceID", "SV_PrimitiveID", "SV_IsFrontFace",
                                 "SV_SampleIndex", "SV_Coverage"})
            if (!_stricmp(element.name.c_str(), made))
                return true;
        return false;
    }
    std::string structure(const char *name, const std::vector<Element> &elements, bool interpolated,
                          const std::string &more) {
        std::string text = std::string("struct ") + name + " {\n";
        for (const bool last : {false, true})
            for (std::size_t field = 0; field < elements.size(); ++field) {
                const auto &element = elements[field];
                if (generated(element) != last)
                    continue;
                const bool integer = element.type == 1 || element.type == 2;
                const bool named = !_strnicmp(element.name.c_str(), "SV_", 3);
                text += "    ";
                if (interpolated && integer && !named)
                    text += "nointerpolation ";
                // a depth that only goes one way is taken with the position at the centroid
                if (interpolated && !_stricmp(element.name.c_str(), "SV_Position"))
                    for (const auto &output : shader_.outputs)
                        if (!_stricmp(output.name.c_str(), "SV_DepthGreaterEqual") ||
                            !_stricmp(output.name.c_str(), "SV_DepthLessEqual")) {
                            text += "linear noperspective centroid ";
                            break;
                        }
                text += field_type(element) + " f" + std::to_string(field) + " : " + element.name;
                if (!named || element.index || !_stricmp(element.name.c_str(), "SV_Target") ||
                    !_stricmp(element.name.c_str(), "SV_ClipDistance") ||
                    !_stricmp(element.name.c_str(), "SV_CullDistance"))
                    text += std::to_string(element.index);
                text += ";\n";
            }
        return text + more + "};\n";
    }
    std::string finish() {
        if (stage_ != Stage::compute && !exported_)
            give_up("nothing exported");
        // A vertex shader that calls no fetch shader reads its vertices itself, from buffers it
        // has descriptors of: it starts with the vertex's index in v0 and the instance's in v3,
        // as one that fetches has them, and takes nothing from the input assembler.
        const bool pulls = stage_ == Stage::vertex && !fetched_ && !shader_.inputs.empty();
        if (pulls)
            prologue_ = "    " + v(0) + " = input.xv_vertex;\n    " + v(3) +
                        " = input.xv_instance;\n" + prologue_;
        std::string text = helpers;
        if (uses_words_)
            text += "struct Words { uint w[" + std::to_string(words_in_element) + "]; };\n";
        if (uses_constants_)
            text += "struct Constants { uint4 w[4096]; };\n";
        if (uses_shared_)
            text += "groupshared uint lds[" + std::to_string(shared_words()) + "];\n";
        if (uses_data_) {
            // the constants a shader keeps after its code, by their address from the first
            // instruction
            const auto count = std::min<std::size_t>(code_.size() - data_first_, 4096);
            if (!count)
                give_up("constants after the code that are not there");
            text += "static const uint code_data[" + std::to_string(count) + "] = {";
            for (std::size_t n = 0; n < count; ++n)
                text += (n ? ", " : "") + hex(code_[data_first_ + n]);
            text += "};\nuint code_word(uint address) { return code_data[min((address >> 2) - " +
                    std::to_string(data_first_) + "u, " + std::to_string(count - 1) + "u)]; }\n";
        }
        for (const auto &[name, size] : constants_size_) {
            const auto mark = "@" + name + "@";
            if (const auto at = declarations_.find(mark); at != std::string::npos)
                declarations_.replace(at, mark.size(), std::to_string(std::min(size, 4096u)));
        }
        text += declarations_;
        // the first descriptor of the table the title draws with, by the stage
        if (uses_heap_) {
            text += std::string("cbuffer xv_user : register(b") +
                    (stage_ == Stage::pixel ? "1" : "0") + ", space200) {";
            for (int n = 0; n < 24; ++n)
                text += " uint xv_ud" + std::to_string(n) + ";";
            text += " };\n";
        }
        std::string registers;
        for (std::size_t n = 0; n < dynamic_id_.size(); ++n)
            registers += "    uint d" + std::to_string(n) + " = 0u;\n";
        for (std::size_t n = 0; n < spills_.size(); ++n)
            registers += "    uint w" + std::to_string(n) + " = 0u;\n";
        for (const auto &[origin, first] : facts_.met)
            if (origin == first)
                registers += "    uint c" + std::to_string(first) + " = 0u;\n";
        for (const auto reg : used_s_)
            registers += "    uint s" + std::to_string(reg) + " = " +
                         (reg == 126 || reg == 127 ? "0xffffffffu" : "0u") + ";\n";
        for (const auto reg : used_v_)
            registers += "    uint v" + std::to_string(reg) + " = 0u;\n";
        registers += "    bool scc = false;\n    uint next = 0u;\n";
        if (!left_.empty())
            registers += "    bool back = false;\n";
        if (stage_ == Stage::mesh) {
            // One thread of the host's group for each lane of the console's, whose number is
            // its vertex and its primitive. The counts are the most the shader makes, and
            // what it leaves unexported is nothing: every primitive starts culled (its
            // corners all the first vertex) and every vertex zero.
            text += structure("Output", shader_.outputs, false, "");
            text += "[outputtopology(\"triangle\")] [numthreads(" + std::to_string(mesh_threads_) +
                    ", 1, 1)]\nvoid main(uint xv_flat : SV_GroupIndex, uint3 xv_group : "
                    "SV_GroupID, out vertices Output xv_verts[" +
                    std::to_string(mesh_vertices_) + "], out indices uint3 xv_prims[" +
                    std::to_string(mesh_primitives_) + "]) {\n    SetMeshOutputCounts(" +
                    std::to_string(mesh_vertices_) + "u, " + std::to_string(mesh_primitives_) +
                    "u);\n    if (xv_flat < " + std::to_string(mesh_primitives_) +
                    "u) xv_prims[xv_flat] = uint3(0u, 0u, 0u);\n    if (xv_flat < " +
                    std::to_string(mesh_vertices_) + "u) xv_verts[xv_flat] = (Output)0;\n" +
                    registers + prologue_ + body_ + "}\n";
            return text;
        }
        if (stage_ == Stage::compute) {
            text += "[numthreads(" + std::to_string(std::max(header_[1], 1u)) + ", " +
                    std::to_string(std::max(header_[2], 1u)) + ", " +
                    std::to_string(std::max(header_[3], 1u)) +
                    ")]\nvoid main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {\n" +
                    registers + prologue_ + body_ + "}\n";
            return text;
        }
        std::string more;
        if (stage_ == Stage::vertex) {
            if (!vertex_id_ || pulls)
                more += "    uint xv_vertex : SV_VertexID;\n";
            if (!instance_id_ || pulls)
                more += "    uint xv_instance : SV_InstanceID;\n";
        }
        static const std::vector<Element> none;
        const auto &inputs = pulls ? none : shader_.inputs;
        const bool no_input = inputs.empty() && more.empty();
        if (!no_input)
            text += structure("Input", inputs, stage_ == Stage::pixel, more);
        if (shader_.outputs.empty()) // a pixel shader that only discards
            return text + "void main(" + (no_input ? "" : "Input input") + ") {\n" + registers +
                   prologue_ + body_ + "}\n";
        text += structure("Output", shader_.outputs, false, "");
        text += std::string("Output main(") + (no_input ? "" : "Input input") +
                ") {\n    Output output = (Output)0;\n" + registers + prologue_ + body_ +
                "    return output;\n}\n";
        return text;
    }

    static constexpr const char *helpers =
        "float mul_legacy(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }\n"
        "float snorm10(float x) { float r = round(x * 1023.0); return max((r >= 512.0 ? r - "
        "1024.0 : r) / 511.0, -1.0); }\n"
        "uint mul_hi(uint a, uint b) { uint al = a & 0xffffu, ah = a >> 16, bl = b & 0xffffu, bh = "
        "b >> 16;\n"
        "    uint ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;\n"
        "    uint mid = (ll >> 16) + (lh & 0xffffu) + (hl & 0xffffu);\n"
        "    return hh + (lh >> 16) + (hl >> 16) + (mid >> 16); }\n"
        "uint mul_hi_signed(uint a, uint b) { uint high = mul_hi(a, b);\n"
        "    if ((int)a < 0) high -= b;\n"
        "    if ((int)b < 0) high -= a;\n"
        "    return high; }\n"
        "float cube_ma(float x, float y, float z) { float3 a = abs(float3(x, y, z)); return 2.0 * "
        "(a.z >= a.x && a.z >= a.y ? z : a.y >= a.x ? y : x); }\n"
        "float cube_id(float x, float y, float z) { float3 a = abs(float3(x, y, z)); return a.z >= "
        "a.x && a.z >= a.y ? (z < 0.0 ? 5.0 : 4.0) : a.y >= a.x ? (y < 0.0 ? 3.0 : 2.0) : (x < 0.0 "
        "? 1.0 : 0.0); }\n"
        "float cube_sc(float x, float y, float z) { float3 a = abs(float3(x, y, z)); return a.z >= "
        "a.x && a.z >= a.y ? (z < 0.0 ? -x : x) : a.y >= a.x ? x : (x < 0.0 ? z : -z); }\n"
        "float cube_tc(float x, float y, float z) { float3 a = abs(float3(x, y, z)); return a.z >= "
        "a.x && a.z >= a.y ? -y : a.y >= a.x ? (y < 0.0 ? -z : z) : -y; }\n"
        "void shift_left(uint lo, uint hi, uint n, out uint rl, out uint rh) { n &= 63u;\n"
        "    if (n == 0u) { rl = lo; rh = hi; } else if (n < 32u) { rl = lo << n; rh = (hi << n) | "
        "(lo >> (32u - n)); } else { rl = 0u; rh = lo << (n - 32u); } }\n"
        "void shift_right(uint lo, uint hi, uint n, out uint rl, out uint rh) { n &= 63u;\n"
        "    if (n == 0u) { rl = lo; rh = hi; } else if (n < 32u) { rl = (lo >> n) | (hi << (32u - "
        "n)); rh = hi >> n; } else { rl = hi >> (n - 32u); rh = 0u; } }\n"
        "void shift_signed(uint lo, uint hi, uint n, out uint rl, out uint rh) { n &= 63u;\n"
        "    if (n == 0u) { rl = lo; rh = hi; } else if (n < 32u) { rl = (lo >> n) | (hi << (32u - "
        "n)); rh = (uint)((int)hi >> n); } else { rl = (uint)((int)hi >> (n - 32u)); rh = "
        "(uint)((int)hi >> 31); } }\n"
        "void field_mask(uint count, uint first, out uint rl, out uint rh) { count &= 63u;\n"
        "    uint lo = count >= 32u ? 0xffffffffu : (1u << count) - 1u, hi = count > 32u ? (1u << "
        "(count - 32u)) - 1u : 0u;\n"
        "    shift_left(lo, hi, first, rl, rh); }\n"
        "uint quad_mask(uint x) { uint r = 0u; for (uint i = 0u; i < 8u; ++i) r |= ((x >> (4u * "
        "i)) & 15u) != 0u ? 1u << i : 0u; return r; }\n"
        "uint bit_pairs(uint x) { x = (x | (x << 8)) & 0x00ff00ffu; x = (x | (x << 4)) & "
        "0x0f0f0f0fu; x = (x | (x << 2)) & 0x33333333u; x = (x | (x << 1)) & 0x55555555u; return "
        "x * 3u; }\n"
        "uint frexp_exponent(uint b) { uint e = (b >> 23) & 255u, m = b & 0x7fffffu;\n"
        "    return e == 255u || (e | m) == 0u ? 0u : e == 0u ? firstbithigh(m) - 148u : e - "
        "126u; }\n"
        "uint frexp_mantissa(uint b) { uint e = (b >> 23) & 255u, m = b & 0x7fffffu;\n"
        "    if (e == 255u || (e | m) == 0u) return b;\n"
        "    if (e == 0u) m = (m << (23u - firstbithigh(m))) & 0x7fffffu;\n"
        "    return (b & 0x80000000u) | 0x3f000000u | m; }\n"
        "uint lerp_bytes(uint a, uint b, uint c) { uint r = 0u; for (uint i = 0u; i < 32u; i += "
        "8u) r |= ((((a >> i) & 255u) + ((b >> i) & 255u) + ((c >> i) & 1u)) >> 1) << i; return "
        "r; }\n"
        "uint sad_bytes(uint a, uint b, bool masked) { uint r = 0u; for (uint i = 0u; i < 32u; i "
        "+= 8u) { int x = (int)((a >> i) & 255u), y = (int)((b >> i) & 255u); if (!masked || y "
        "!= 0) r += (uint)abs(x - y); } return r; }\n"
        "uint perm_byte(uint hi, uint lo, uint s) { s &= 255u; if (s < 4u) return (lo >> (8u * "
        "s)) & 255u; if (s < 8u) return (hi >> (8u * (s - 4u))) & 255u;\n"
        "    if (s < 12u) return (((s < 10u ? lo : hi) >> (16u * (s & 1u) + 15u)) & 1u) != 0u ? "
        "255u : 0u; return s == 12u ? 0u : 255u; }\n"
        "uint perm_bytes(uint hi, uint lo, uint s) { return perm_byte(hi, lo, s) | (perm_byte(hi, "
        "lo, s >> 8) << 8) | (perm_byte(hi, lo, s >> 16) << 16) | (perm_byte(hi, lo, s >> 24) << "
        "24); }\n"
        "float3 cube_direction(float3 place) { float2 at = (place.xy - 1.5) * 2.0; int face = "
        "(int)round(place.z);\n"
        "    return face == 0 ? float3(1.0, -at.y, -at.x) : face == 1 ? float3(-1.0, -at.y, at.x) "
        ": face == 2 ? float3(at.x, 1.0, at.y)\n"
        "         : face == 3 ? float3(at.x, -1.0, -at.y) : face == 4 ? float3(at.x, -at.y, 1.0) : "
        "float3(-at.x, -at.y, -1.0); }\n"
        "bool float_class(float x, uint mask) { uint b = asuint(x); bool neg = (b >> 31) != 0u; "
        "uint e = (b >> 23) & 255u; uint m = b & 0x7fffffu;\n"
        "    uint c = e == 255u ? (m != 0u ? ((m >> 22) != 0u ? 2u : 1u) : (neg ? 4u : 512u)) : e "
        "== 0u ? (m == 0u ? (neg ? 32u : 64u) : (neg ? 16u : 128u)) : (neg ? 8u : 256u);\n"
        "    return (mask & c) != 0u; }\n"
        "bool double_class(uint lo, uint hi, uint mask) { bool neg = (hi >> 31) != 0u; uint e = "
        "(hi >> 20) & 2047u; bool none = ((hi & 0xfffffu) | lo) == 0u;\n"
        "    uint c = e == 2047u ? (!none ? (((hi >> 19) & 1u) != 0u ? 2u : 1u) : (neg ? 4u : "
        "512u)) : e == 0u ? (none ? (neg ? 32u : 64u) : (neg ? 16u : 128u)) : (neg ? 8u : 256u);\n"
        "    return (mask & c) != 0u; }\n";

    Stage stage_;
    const Shader &shader_;
    Facts &facts_;
    std::map<std::string, std::pair<std::string, std::uint32_t>> arrays_; // name and first slot
    std::set<std::pair<std::uint32_t, std::size_t>> taken_;               // ranges with an array
    std::optional<std::pair<std::uint32_t, std::uint32_t>> placed_;
    std::map<std::string, std::uint32_t> constants_size_; // in vectors, by name
    bool uses_words_ = false, uses_constants_ = false, uses_data_ = false, quad_ = false;
    bool uses_shared_ = false;
    // what the shader did last with the memory its group shares
    enum class Shared { settled, read, written } shared_ = Shared::settled;
    std::vector<Parameter> parameters_;
    std::vector<std::uint32_t> code_;
    std::uint32_t header_[4] = {};
    std::vector<Ins> code_list_;
    std::map<std::uint32_t, std::size_t> index_of_; // by an instruction's place, in words
    std::uint32_t data_first_ = 0;                  // the word after the last instruction
    std::vector<Block> blocks_;
    std::vector<std::size_t> target_block_;
    std::map<std::size_t, std::size_t> loop_end_;
    std::vector<std::size_t> open_loops_;
    std::set<std::size_t> left_; // loops that a branch back to a loop around them leaves
    State known_{};
    std::vector<State> entry_;                // what is known where each block starts
    std::map<std::uint32_t, int> dynamic_id_; // by the instruction that loads
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> spills_; // register, lane
    std::set<std::uint32_t> wanted_;
    std::map<std::uint32_t, std::uint32_t> attributes_;
    std::set<std::uint32_t> used_s_, used_v_;
    std::map<std::string, std::string> declared_;
    std::string declarations_, prologue_, body_, indent_ = "    ";
    int temporaries_ = 0;
    bool exec_changes_ = false, wave64_ = false;
    bool older_ = false;                           // an Xbox One shader
    std::vector<Placed> in_memory_, in_registers_; // its root parameters
    bool sums_lanes_ = false; // a compute shader with instructions that read other lanes
    bool unlearned_ = false; // an instruction was left out for a descriptor of a kind not known yet
    bool needs_sm6_ = false, sopk_writes_exec_ = false;
    bool fetched_ = false, exported_ = false, vertex_id_ = false, instance_id_ = false;
    bool uses_heap_ = false;
    // a mesh shader's group, outputs (parameter, row) and wave
    std::uint32_t mesh_threads_ = 0, mesh_vertices_ = 0, mesh_primitives_ = 0, mesh_wave_ = 64,
                  mesh_shared_ = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> mesh_outputs_;
    std::vector<Shader::Bound> bound_;                      // the caller's, or the file's own
    std::map<std::uint32_t, std::uint32_t> table_register_; // a table parameter's register
    std::map<std::uint32_t, std::uint32_t>
        heap_parameter_; // a heap as a table: 1 views, 2 samplers
};
} // namespace

std::optional<std::string> translate(Stage stage, const Shader &shader, std::string *why,
                                     std::uint32_t *at, bool *needs_sm6) {
    if (needs_sm6)
        *needs_sm6 = false;
    if (stage != Stage::vertex && stage != Stage::pixel && stage != Stage::compute &&
        stage != Stage::mesh) {
        if (why)
            *why = "stage";
        return std::nullopt;
    }
    try {
        // not on the stack: it is large, and a title's threads may have little; and again
        // with what a pass found out, until a pass finds nothing new
        Facts facts;
        for (int pass = 0; pass < 64; ++pass) {
            try {
                const auto translator = std::make_unique<Translator>(stage, shader, facts);
                auto source = translator->run();
                if (needs_sm6)
                    *needs_sm6 = translator->needs_sm6();
                return source;
            } catch (const Retry &) {
            }
        }
        give_up("descriptors that do not settle");
    } catch (const Unsupported &unsupported) {
        if (why)
            *why = unsupported.what;
        if (at)
            *at = unsupported.at;
    } catch (const std::exception &error) {
        if (why)
            *why = error.what();
    }
    return std::nullopt;
}
} // namespace xv::runtime::modules::rdna
