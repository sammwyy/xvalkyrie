#include "runtime/modules/console_shader.hpp"
#include "runtime/modules/console_shader_rdna.hpp"
#include <catch2/catch.hpp>
#include <cctype>
#include <chrono>
#include <cstring>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace {
using namespace xv::runtime::modules;
constexpr std::uint32_t v1 = 0x101, v2 = 0x102, v3 = 0x103, end_program = 0xbf810000;
// the translation of a compute shader of these words, which the host's compiler must take
std::string translated(std::initializer_list<std::uint32_t> words) {
    std::vector<std::uint32_t> code(words);
    code.push_back(end_program);
    std::string source;
    const auto why = console_shader_probe(code, true, &source);
    INFO(source);
    REQUIRE(why.empty());
    return source;
}
std::uint32_t shared(std::uint32_t operation) { return 0x36u << 26 | operation << 18; }
constexpr std::uint32_t shared_registers = 1 | 2 << 8 | 3 << 16 | 4 << 24;
std::size_t count(const std::string &text, const std::string &what) {
    std::size_t found = 0;
    for (auto at = text.find(what); at != std::string::npos; at = text.find(what, at + 1))
        ++found;
    return found;
}
// what the last statement that writes the register `reg` gives it
std::string last_set(const std::string &source, const std::string &reg) {
    const auto at = source.rfind(" " + reg + " = ");
    REQUIRE(at != std::string::npos);
    const auto from = at + reg.size() + 4;
    return source.substr(from, source.find(';', from) - from);
}
// whether `text` reads the register `reg`
bool reads(const std::string &text, const std::string &reg) {
    for (auto at = text.find(reg); at != std::string::npos; at = text.find(reg, at + 1)) {
        const auto after = at + reg.size();
        if ((!at || !std::isalnum(static_cast<unsigned char>(text[at - 1]))) &&
            (after == text.size() || !std::isdigit(static_cast<unsigned char>(text[after]))))
            return true;
    }
    return false;
}

std::optional<std::string> pixel_move(std::uint32_t move, std::uint32_t selectors, std::string &why,
                                      bool &needs_sm6) {
    // LLVM gfx1013: v_mov_b32 v20, 1.0; v_mov_b32_dpp v24, v20; exp mrt0, v24, off, off, off.
    const std::uint32_t code[] = {0x7e2802f2, move, selectors, 0xf8001801, 24, end_program};
    constexpr std::size_t region = 0x140, header = 0x28;
    std::vector<std::uint8_t> file(region + header + sizeof code);
    const auto put = [&](std::size_t offset, std::uint32_t value) {
        std::memcpy(file.data() + offset, &value, sizeof value);
    };
    std::memcpy(file.data(), "FSSM", 4);
    put(0xc0, region);
    put(0xc8, header + sizeof code);
    std::memcpy(file.data() + region + header, code, sizeof code);
    const std::uint32_t root[] = {1, 0, 24, 0, 0, 0};
    const rdna::Shader shader{file,
                              {reinterpret_cast<const std::uint8_t *>(root), sizeof root},
                              {},
                              {{"SV_Target", 0, 3, 4, true, 0, 15, 0}}};
    return rdna::translate(ShaderStage::pixel, shader, &why, nullptr, &needs_sm6);
}
// The translation of a pixel shader of these words, which the host's compiler must take.
// Before them, as console_shader_probe has for a compute shader: a descriptor of the first
// table in s[12:19] and a sampler of the second in s[24:27]. With `needs_sm6`, whether it
// takes Shader Model 6, and then the older compiler is not asked.
std::string pixel_translated(std::initializer_list<std::uint32_t> words,
                             bool *needs_sm6 = nullptr) {
    std::vector<std::uint32_t> code = {
        0xf40c0304, 0xfa000000, // s_load_dwordx8 s[12:19], s[8:9], 0
        0xbe940309,             // s_mov_b32 s20, s9
        0xf408060a, 0xfa000000, // s_load_dwordx4 s[24:27], s[20:21], 0
    };
    code.insert(code.end(), words);
    code.push_back(end_program);
    constexpr std::size_t region = 0x140, header = 0x28;
    std::vector<std::uint8_t> file(region + header + 4 * code.size());
    const auto put = [&](std::size_t offset, std::uint32_t value) {
        std::memcpy(file.data() + offset, &value, sizeof value);
    };
    std::memcpy(file.data(), "FSSM", 4);
    put(0xc0, region);
    put(0xc8, static_cast<std::uint32_t>(header + 4 * code.size()));
    std::memcpy(file.data() + region + header, code.data(), 4 * code.size());
    // console_shader_probe's root signature: the header and two tables, of shader resources
    // without end from t0 and of sixteen samplers from s0
    static constexpr std::uint32_t root[] = {
        1, 2,  24, 0,   48, 0, 0, 0, 48, 0, 0, 76, // header, parameters
        1, 56, 0,  ~0u, 0,  0, 0,                  // shader resources
        1, 84, 3,  16,  0,  0, 0,                  // samplers
    };
    const rdna::Shader shader{file,
                              {reinterpret_cast<const std::uint8_t *>(root), sizeof root},
                              {},
                              {{"SV_Target", 0, 3, 4, true, 0, 15, 0}}};
    std::string why;
    const auto source = rdna::translate(ShaderStage::pixel, shader, &why, nullptr, needs_sm6);
    INFO(why);
    REQUIRE(source);
    INFO(*source);
    if (!needs_sm6)
        CHECK(console_shader_errors(ShaderStage::pixel, *source).empty());
    return *source;
}
} // namespace

TEST_CASE("a group's threads wait for each other between writing and reading its memory") {
    // ds_write_b32, ds_read_b32, ds_read_b32, ds_add_rtn_u32
    const auto source =
        translated({shared(0xd), shared_registers, shared(0x36), shared_registers, shared(0x36),
                    shared_registers, shared(0x20), shared_registers});
    CHECK(source.find("groupshared uint lds[8192];") != std::string::npos);
    CHECK(count(source, "GroupMemoryBarrierWithGroupSync();") == 2);
    CHECK(source.find("InterlockedAdd(lds[") != std::string::npos);
}

TEST_CASE("the shader's own barrier is the host's") {
    // ds_write_b32, s_barrier, ds_read_b32: one wait, not two
    const auto source =
        translated({shared(0xd), shared_registers, 0xbf8a0000, shared(0x36), shared_registers});
    CHECK(count(source, "GroupMemoryBarrierWithGroupSync();") == 1);
}

TEST_CASE("instructions on parts of memory and registers compile") {
    // ds_write_b8, ds_cmpst_rtn_b32, ds_read2_b32
    translated({shared(0x1e), shared_registers, shared(0x30), shared_registers, shared(0x37),
                shared_registers});
    // s_flbit_i32_i64 s1, s[2:3]; s_quadmask_b64 s[4:5], s[2:3]
    translated({0x17du << 23 | 1 << 16 | 0x18 << 8 | 2, 0x17du << 23 | 4 << 16 | 0x2d << 8 | 2});
    // v_pk_add_i16 v1, v1, v2; v_sad_u8 v1, v1, v2, v3; v_alignbit_b32 v1, v1, v2, v3
    translated({0x33u << 26 | 2 << 16 | 1, v1 | v2 << 9, 0x35u << 26 | 0x15a << 16 | 1,
                v1 | v2 << 9 | v3 << 18, 0x35u << 26 | 0x14e << 16 | 1, v1 | v2 << 9 | v3 << 18});
}

TEST_CASE("operations on a group's memory the host has no function for retry until they hold") {
    // ds_dec_u32, ds_min_f32, ds_mskor_rtn_b32, ds_wrap_rtn_b32
    const auto source =
        translated({shared(0x4), shared_registers, shared(0x12), shared_registers, shared(0x2c),
                    shared_registers, shared(0x34), shared_registers});
    CHECK(count(source, "do { X = lds[") == 4);
    CHECK(count(source, "InterlockedCompareExchange(lds[") == 4);
    // ds_wrxchg2_rtn_b32, ds_read_b96, ds_write_b8_d16_hi, ds_read_u16_d16_hi
    const auto parts = translated({shared(0x2e), shared_registers, shared(0xfe), shared_registers,
                                   shared(0xa0), shared_registers, shared(0xa7), shared_registers});
    CHECK(count(parts, "InterlockedExchange(lds[") == 2);
    CHECK(parts.find("(v2 >> 16) & 0xffu") != std::string::npos);
    CHECK(parts.find("v4 = (v4 & 0xffffu) | (") != std::string::npos);
}

TEST_CASE("64-bit values are doubles of the host's and pairs of words") {
    // v_cmp_lt_f64 vcc, 1.0, v[2:3]; v_cvt_i32_f64 v1, v[2:3]; v_cmp_class_f64 vcc, v[1:2], v2
    const auto compared =
        translated({0x3eu << 25 | 0x21 << 17 | 2 << 9 | 242, 0x3fu << 25 | 1 << 17 | 0x3 << 9 | v2,
                    0x3eu << 25 | 0xa8 << 17 | 2 << 9 | v1});
    CHECK(compared.find("asdouble(0x00000000u, 0x3ff00000u) < asdouble(v2, v3)") !=
          std::string::npos);
    CHECK(compared.find("double_class(v1, v2, v2)") != std::string::npos);
    // v_fma_f64 v[4:5], |v[1:2]|, v[2:3], literal; v_cmp_ge_u64 s106, v[1:2], -1
    const auto computed =
        translated({0x35u << 26 | 0x14c << 16 | 1 << 8 | 4, v1 | v2 << 9 | 255 << 18, 0x40080000,
                    0x35u << 26 | 0xe6 << 16 | 106, v1 | 193 << 9});
    CHECK(computed.find("fma(asdouble(v1, (v2 & 0x7fffffffu)), asdouble(v2, v3), asdouble(0u, "
                        "0x40080000u))") != std::string::npos);
    CHECK(computed.find("!(v2 < 0xffffffffu || (v2 == 0xffffffffu && v1 < 0xffffffffu))") !=
          std::string::npos);
}

TEST_CASE("a wave is this one lane where its lanes are counted") {
    // s_mov_b64 s[8:9], exec; s_bcnt1_i32_b64 s4, s[8:9]; v_mbcnt_lo_u32_b32 v1, -1, v2
    const auto source = translated({0xbe88047e, 0xbe841008, 0xd7650001, 0x000204c1});
    CHECK(source.find("s4 = (s8 != 0u ? 1u : 0u);") != std::string::npos);
    CHECK(source.find("v1 = v2;") != std::string::npos);
    // s_mov_b32 s8, 7; s_bcnt1_i32_b64 s4, s[8:9]: a number's bits are counted
    const auto number = translated({0xbe880387, 0xbe841008});
    CHECK(number.find("countbits(s8)") != std::string::npos);
}

TEST_CASE("a wave is of 64 lanes where its masks are, not where it moves addresses in pairs") {
    // v_cmp_lt_f32_e64 s20, 0, v1; s_mov_b64 s[6:7], s[0:1]: a wave of 32, whose compare
    // writes one register
    auto source = translated({0xd4010014, 0x00020280, 0xbe860400});
    CHECK(source.find("s21 = s20;") == std::string::npos);
    // and s_and_saveexec_b64 s[8:9], s[20:21] after it: a wave of 64, whose masks are pairs
    source = translated({0xd4010014, 0x00020280, 0xbe860400, 0xbe882414});
    CHECK(source.find("s21 = s20;") != std::string::npos);
}

TEST_CASE("a sum over a wave's lanes is this lane's own value") {
    // v_add_nc_u32_dpp8 v2, v2, v2 from the lane beside; v_add_nc_u32_dpp v3, v3, v3 from the
    // lane before, with zero for none, and from two before, with nothing for none;
    // v_permlanex16_b32 v4, v3, -1, -1; v_readlane_b32 s3, v3, 31; s_mov_b32 exec_lo, 0xffff0000
    const auto source =
        translated({0x4a0404e9, 0xde54c102, 0x4a0606fa, 0xff091103, 0x4a0606fa, 0xff011203,
                    0xd7784804, 0x03058303, 0xd7600003, 0x00013f03, 0xbefe03ff, 0xffff0000});
    CHECK(source.find("v2 = 0u + v2;") != std::string::npos);
    CHECK(count(source, "v3 = 0u + v3;") == 1);
    CHECK(source.find("v4 = 0u;") != std::string::npos);
    CHECK(source.find("s3 = 0u;") != std::string::npos);
    CHECK(source.find("s126 = 0u;") != std::string::npos);
}

TEST_CASE("a register picked by M0 and bytes picked by a selector") {
    // v_movreld_b32 v18, v55; v_perm_b32 v1, v1, v2, v3
    const auto source =
        translated({0x7e248537, 0x35u << 26 | 0x344 << 16 | 1, v1 | v2 << 9 | v3 << 18});
    // from v18 on the registers are an array, which M0 indexes
    CHECK(source.find("uint vr[238];") != std::string::npos);
    CHECK(source.find("vr[min(0u + s124, 237u)] = vr[37];") != std::string::npos);
    CHECK(source.find("switch (s124)") == std::string::npos);
    CHECK(source.find("v1 = perm_bytes(v1, v2, v3);") != std::string::npos);
}

TEST_CASE("a register that holds one of two descriptors is used as each") {
    // s_load_dwordx8 s[12:19], s[4:5], 0x20; s_cbranch_scc0 over the next;
    // s_load_dwordx8 s[12:19], s[4:5], 0x40; image_load v3, v[0:1], s[12:19]
    const auto source = translated(
        {0xf40c0302, 0xfa000020, 0xbf840002, 0xf40c0302, 0xfa000040, 0xf0001108, 0x00030300});
    CHECK(count(source, "if (c5 == ") == 2);
    CHECK(source.find("c5 = 5u;") != std::string::npos);
    CHECK(source.find("c5 = 8u;") != std::string::npos);
    CHECK(source.find("t1_0.Load(") != std::string::npos);
    CHECK(source.find("t2_0.Load(") != std::string::npos);
}

TEST_CASE("a typed buffer load reads its element by the format the instruction names") {
    // tbuffer_load_format_xy v[1:2], v0, s[12:15], format:[BUF_FMT_32_32_FLOAT] idxen
    const auto words = translated({0xea012000, 0x80030100});
    CHECK(reads(last_set(words, "v1"), "v0") == false);
    CHECK(words.find(".w[0]") != std::string::npos);
    CHECK(words.find(".w[1]") != std::string::npos);
    // tbuffer_load_format_xyzw v[1:4], v0, s[12:15], format:[BUF_FMT_8_8_8_8_UNORM] idxen
    const auto bytes = translated({0xe9c32000, 0x80030100});
    CHECK(last_set(bytes, "v4").find("/ 255.0") != std::string::npos);
    CHECK(last_set(bytes, "v1").find("& 0x000000ffu") != std::string::npos);
    // tbuffer_load_format_x v1, v0, s[12:15], format:[BUF_FMT_16_FLOAT] idxen
    const auto half = translated({0xe8682000, 0x80030100});
    CHECK(last_set(half, "v1").find("f16tof32") != std::string::npos);
}

TEST_CASE("a load into the registers of its address reads every word from the address given") {
    // buffer_load_dwordx4 v[1:4], v[1:2], s[12:15], 0 idxen offen
    const auto buffer = translated({0xe0383000, 0x80030101});
    for (const auto *reg : {"v1", "v2", "v3", "v4"}) {
        const auto value = last_set(buffer, reg);
        INFO(reg << " = " << value);
        CHECK_FALSE(reads(value, "v1"));
        CHECK_FALSE(reads(value, "v2"));
    }
    // s_buffer_load_dwordx2 s[0:1], s[12:15], s0 offset:0x10
    const auto scalar = translated({0xf4240006, 0x00000010});
    CHECK_FALSE(reads(last_set(scalar, "s1"), "s0"));
    // s_getpc_b64 s[0:1]; s_load_dwordx2 s[0:1], s[0:1], 0xc; s_endpgm; and the two words it
    // reads, 0xc bytes from the s_load_dwordx2
    const auto code =
        translated({0xbe801f00, 0xf4040000, 0xfa00000c, end_program, 0x3f800000, 0x40000000});
    CHECK(code.find("code_word(") != std::string::npos);
    CHECK_FALSE(reads(last_set(code, "s1"), "s0"));
}

TEST_CASE("an exchange of two words of a group's memory returns them once both are taken") {
    // ds_wrxchg2_rtn_b32 v[3:4], v1, v2, v3 offset1:1: the first returned goes to v3
    const auto source = translated({shared(0x2e) | 1 << 8, 1 | 2 << 8 | 3 << 16 | 3 << 24});
    CHECK(count(source, "InterlockedExchange(lds[") == 2);
    CHECK(source.find("], v3, ") < source.rfind(" v3 = "));
}

TEST_CASE("both levels of detail are found before either is written") {
    // image_get_lod v[0:1], v[0:1], s[12:19], s[24:27] dmask:0x3 dim:SQ_RSRC_IMG_2D;
    // exp mrt0 v0, v1, off, off done vm
    const auto source = pixel_translated({0xf1800308, 0x00c30000, 0xf8001803, 0x00000100});
    CHECK(source.find("CalculateLevelOfDetailUnclamped(") != std::string::npos);
    CHECK_FALSE(reads(last_set(source, "v1"), "v0"));
}

TEST_CASE("a rectangle list's fourth corner comes from a geometry shader") {
    const char vertex[] = "struct Out { float4 position : SV_Position; float2 uv : TEXCOORD0; "
                          "nointerpolation uint layer : LAYER0; };\n"
                          "Out main(uint id : SV_VertexID) { Out o; o.position = float4(id & 1, "
                          "id >> 1, 0, 1); o.uv = o.position.xy; o.layer = id; return o; }\n";
    ID3DBlob *code = nullptr;
    REQUIRE(D3DCompile(vertex, sizeof vertex - 1, nullptr, nullptr, nullptr, "main", "vs_5_1", 0, 0,
                       &code, nullptr) == S_OK);
    const auto made = rectangle_shader(
        {static_cast<const std::uint8_t *>(code->GetBufferPointer()), code->GetBufferSize()});
    REQUIRE(made.size() > 4);
    CHECK(!std::memcmp(made.data(), "DXBC", 4));
    // its inputs are the vertex shader's outputs
    ID3D12ShaderReflection *reflection = nullptr;
    REQUIRE(D3DReflect(made.data(), made.size(), IID_PPV_ARGS(&reflection)) == S_OK);
    D3D12_SHADER_DESC description{};
    REQUIRE(reflection->GetDesc(&description) == S_OK);
    CHECK(description.InputParameters == 3);
    CHECK(description.OutputParameters == 3);
    CHECK(description.GSMaxOutputVertexCount == 4);
    reflection->Release();
    code->Release();
    const std::uint8_t nothing[16] = {};
    CHECK(rectangle_shader(nothing).empty());
}

TEST_CASE("a pixel DPP8 move can rearrange the four lanes of each quad") {
    bool needs_sm6 = false;
    std::string why;
    // LLVM gfx1013: dpp8:[1,2,3,0,5,6,7,4].
    const auto source = pixel_move(0x7e3002e9, 0x9f50d114, why, needs_sm6);
    INFO(why);
    REQUIRE(source);
    CHECK(needs_sm6);
    CHECK(count(*source, "QuadReadLaneAt(") == 4);
    CHECK(source->find("WaveGetLaneIndex()") != std::string::npos);
    CHECK(source->find("IsHelperLane() ? 0u : v20") != std::string::npos);
    CHECK(source->find("src, 1u)") != std::string::npos);
    CHECK(source->find("src, 0u)") != std::string::npos);

    CHECK_FALSE(pixel_move(0x7e3002e9, 0x9f50d514, why, needs_sm6)); // lane 0 reads lane 5
    CHECK_FALSE(needs_sm6);
    CHECK_FALSE(pixel_move(0x7e3002ea, 0x9f50d114, why, needs_sm6)); // fetch inactive lanes
    CHECK_FALSE(needs_sm6);
}

TEST_CASE("a pixel DPP16 move reads the lane of its quad that quad_perm names") {
    bool needs_sm6 = false;
    std::string why;
    // v_mov_b32_dpp v24, v20 quad_perm:[1,0,3,2] row_mask:0xf bank_mask:0xf
    auto source = pixel_move(0x7e3002fa, 0xff00b114, why, needs_sm6);
    INFO(why);
    REQUIRE(source);
    CHECK(needs_sm6);
    CHECK(source->find("QuadReadAcrossX(v20)") != std::string::npos);
    // quad_perm:[0,0,2,2]: each lane finds its place in the quad
    source = pixel_move(0x7e3002fa, 0xff00a014, why, needs_sm6);
    REQUIRE(source);
    CHECK(count(*source, "QuadReadLaneAt(") == 7);
    // a row left out is not taken
    CHECK_FALSE(pixel_move(0x7e3002fa, 0x0f00b114, why, needs_sm6));
}

namespace {
// a program of these words in an FSSM file of its own, with nothing in its tables
std::vector<std::uint8_t> program(std::initializer_list<std::uint32_t> words) {
    std::vector<std::uint32_t> code(words);
    code.push_back(end_program);
    constexpr std::size_t region = 0x140, header = 0x28;
    std::vector<std::uint8_t> file(region + header + 4 * code.size());
    const auto put = [&](std::size_t offset, std::uint32_t value) {
        std::memcpy(file.data() + offset, &value, sizeof value);
    };
    std::memcpy(file.data(), "FSSM", 4);
    put(0xc0, region);
    put(0xc8, static_cast<std::uint32_t>(header + 4 * code.size()));
    std::memcpy(file.data() + region + header, code.data(), 4 * code.size());
    return file;
}
} // namespace

TEST_CASE("16-bit inputs and outputs are packed two to a word, a register from a word of its own") {
    const std::uint32_t root[] = {1, 0, 24, 0, 0, 0};
    const std::span<const std::uint8_t> signature{reinterpret_cast<const std::uint8_t *>(root),
                                                  sizeof root};
    // two registers of four halves, which share one attribute: x and y in its first word, z and
    // w in the next, then the second register's
    const std::vector<rdna::Element> halves{{"TEXCOORD", 0, 6, 4, false, 1, 15, 15},
                                            {"TEXCOORD", 1, 6, 4, false, 2, 15, 15}};
    // v_interp_p1ll_f16 v2, v0, attr0.z high; v_interp_p2_f16 v2, v1, attr0.z, v2 high;
    // exp mrt0 v2, off, off, off done
    const auto pixel = program({0xd7420002, 0x00020180, 0xd75a0002, 0x040a0380, 0xf8001801, 2});
    const rdna::Shader shader{pixel, signature, halves, {{"SV_Target", 0, 3, 4, true, 0, 15, 0}}};
    std::string why;
    auto source = rdna::translate(ShaderStage::pixel, shader, &why);
    INFO(why);
    REQUIRE(source);
    INFO(*source);
    CHECK(last_set(*source, "v2") == "f32tof16(input.f1.y)");
    CHECK(console_shader_errors(ShaderStage::pixel, *source).empty());

    // exp pos0 v0, v0, v0, v0; exp param0 v4, v5, v6, v7 done
    const auto vertex = program({0xf80000cf, 0x00000000, 0xf800020f, 0x07060504});
    std::vector<rdna::Element> outputs{{"SV_Position", 0, 3, 4, true, 0, 15, 0}};
    outputs.insert(outputs.end(), halves.begin(), halves.end());
    const rdna::Shader exporting{vertex, signature, {}, outputs};
    source = rdna::translate(ShaderStage::vertex, exporting, &why);
    INFO(why);
    REQUIRE(source);
    INFO(*source);
    CHECK(source->find("output.f1.y = f16tof32((v4 >> 16 & 0xffffu));") != std::string::npos);
    CHECK(source->find("output.f2.w = f16tof32((v7 >> 16 & 0xffffu));") != std::string::npos);
    CHECK(console_shader_errors(ShaderStage::vertex, *source).empty());
}

TEST_CASE("a swizzle outside a pixel quad reads the lane its offset names") {
    // ds_swizzle_b32 v3, v2 offset:swizzle(SWAP,16), the value in its address field; exp mrt0
    // v3, off, off, off done
    bool needs_sm6 = false;
    const auto source =
        pixel_translated({0xd8d4401f, 0x03000002, 0xf8001801, 0x00000003}, &needs_sm6);
    CHECK(needs_sm6);
    CHECK(source.find("WaveReadLaneAt(v2, ") != std::string::npos);
    CHECK(source.find("& ~31u) | (((") != std::string::npos);
    CHECK(source.find("0x00000010u)") != std::string::npos);
}

TEST_CASE("a swizzle in a compute shader takes its wave as the one lane it is run for") {
    // ds_swizzle_b32 v1, v2 offset:swizzle(SWAP,16); ds_swizzle_b32 v3, v2
    // offset:swizzle(BROADCAST,32,0); ds_swizzle_b32 v4, v2 offset:swizzle(QUAD_PERM,1,0,3,2);
    // ds_swizzle_b32 v5, v2 offset:swizzle(QUAD_PERM,0,0,0,0)
    const auto source = translated({0xd8d4401f, 0x01000002, 0xd8d40000, 0x03000002, 0xd8d480b1,
                                    0x04000002, 0xd8d48000, 0x05000002});
    CHECK(source.find("WaveReadLaneAt(") == std::string::npos);
    CHECK(last_set(source, "v1") == "0u");
    CHECK(last_set(source, "v3") == "v2");
    CHECK(last_set(source, "v4") == "0u");
    CHECK(last_set(source, "v5") == "v2");
    // an offset with bit 15 and more set is a mode the translator does not know
    CHECK_FALSE(console_shader_probe(std::vector<std::uint32_t>{0xd8d4e01f, 0x03000004}).empty());
}

TEST_CASE("a swizzle between the pixels of a quad reads the register in its address field") {
    // ds_swizzle_b32 v3, v2 offset:swizzle(QUAD_PERM,0,0,0,0); exp mrt0 v3, off, off, off done
    const auto source = pixel_translated({0xd8d48000, 0x03000002, 0xf8001801, 0x00000003});
    CHECK(source.find("= asfloat(v2);") != std::string::npos);
}

TEST_CASE("a vertex shader that calls no fetch shader reads its vertex and instance itself") {
    // v_cvt_f32_u32 v1, v0; v_cvt_f32_u32 v2, v3; exp pos0 v1, v2, v1, v1 done
    const std::uint32_t code[] = {0x7e020d00, 0x7e040d03, 0xf80008cf, 0x01010201, end_program};
    constexpr std::size_t region = 0x140, header = 0x28;
    std::vector<std::uint8_t> file(region + header + sizeof code);
    const auto put = [&](std::size_t offset, std::uint32_t value) {
        std::memcpy(file.data() + offset, &value, sizeof value);
    };
    std::memcpy(file.data(), "FSSM", 4);
    put(0xc0, region);
    put(0xc8, header + sizeof code);
    std::memcpy(file.data() + region + header, code, sizeof code);
    const std::uint32_t root[] = {1, 0, 24, 0, 0, 0};
    const rdna::Shader shader{file,
                              {reinterpret_cast<const std::uint8_t *>(root), sizeof root},
                              {{"POSITION", 0, 3, 3, false, 0, 7, 7}},
                              {{"SV_Position", 0, 3, 4, true, 0, 15, 0}}};
    std::string why;
    const auto source = rdna::translate(ShaderStage::vertex, shader, &why);
    INFO(why);
    REQUIRE(source);
    INFO(*source);
    CHECK(console_shader_errors(ShaderStage::vertex, *source).empty());
    CHECK(source->find("v0 = input.xv_vertex;") != std::string::npos);
    CHECK(source->find("v3 = input.xv_instance;") != std::string::npos);
    CHECK(source->find("POSITION") == std::string::npos);
}

TEST_CASE("a start reads only the journal's entries its index does not know") {
    const auto folder = std::filesystem::temp_directory_path() /
                        ("xv_precompile_" + std::to_string(GetCurrentProcessId()));
    const auto journal = folder / "journal";
    std::filesystem::create_directories(journal);
    // "XVSJ", version 1, a pixel shader for Shader Model 5 without heaps or signed inputs, of
    // four bytes that are no shader, without a root signature
    const std::uint32_t entry[] = {0x4a535658u, 1, 1, 0, 0, 0, 4, 0, 0x65706f6e};
    std::ofstream(journal / "entry.xvsj", std::ios::binary)
        .write(reinterpret_cast<const char *>(entry), sizeof entry);
    const auto run = [&] {
        const auto work = precompile_shaders(folder, journal, 2);
        for (int waited = 0; !work->finished && waited < 1000; ++waited)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        REQUIRE(work->finished);
        CHECK(work->journal == 1);
        CHECK(work->looked == 1);
        return work->total.load();
    };
    const auto noted = [&] {
        for (const auto &item : std::filesystem::directory_iterator(folder))
            if (item.path().extension() == ".none")
                return item.path();
        return std::filesystem::path{};
    };
    CHECK(run() == 1);
    REQUIRE(!noted().empty());
    // settled by this version, as the index says: neither the entry nor the cache is read
    std::filesystem::remove(noted());
    CHECK(run() == 0);
    std::filesystem::remove(journal / "index.txt");
    CHECK(run() == 1);
    std::filesystem::remove_all(folder);
}
