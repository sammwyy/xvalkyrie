#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Shaders a console build ships without the bitcode the host runs: their DXIL part holds
// machine code for the console's GPU instead (see docs/research/console-shaders.md). A host
// shader is made for each one and kept in the shader cache, by the hash of the original.
namespace xv::runtime::modules {
// geometry_points, _lines and _triangles: a geometry shader, by what it is given
enum class ShaderStage {
    vertex,
    pixel,
    compute,
    geometry_points,
    geometry_lines,
    geometry_triangles,
    mesh
};
// whether the container's DXIL part has no bitcode
bool console_only_shader(std::span<const std::uint8_t> container);
bool shader_has_dxil(std::span<const std::uint8_t> container);
// The machine code of such a shader, as little-endian words up to and including the one
// that ends the program. Empty if it is not found.
std::vector<std::uint32_t> console_shader_code(std::span<const std::uint8_t> container);
// The HLSL made for a console shader, or nothing and in `why` what stopped it, at the
// instruction `at` words into its code (~0 if at none). For tools and tests: `host_shader`
// is what a title's pipelines get.
std::optional<std::string> console_shader_source(ShaderStage stage,
                                                 std::span<const std::uint8_t> container,
                                                 std::string *why = nullptr,
                                                 std::uint32_t *at = nullptr,
                                                 std::span<const std::uint8_t> root_signature = {});
// what the host's compiler says against such HLSL, or nothing if it takes it
std::string console_shader_errors(ShaderStage stage, const std::string &source);
// What stops the translation of a compute shader made of these words of machine code and
// nothing else, or nothing if it is translated. For tools: which instructions are handled.
// With `compiled`, what is translated is given to the host's compiler too, and what that
// says against it is the answer. `source` gets the translation.
std::string console_shader_probe(std::span<const std::uint32_t> words, bool compiled = false,
                                 std::string *source = nullptr);
// The HLSL for a vertex or pixel program of a pipeline of the console's cache, which is the
// machine code alone; `why` says what stops it.
std::optional<std::string> console_program_source(ShaderStage stage,
                                                  std::span<const std::uint32_t> words,
                                                  std::span<const std::uint8_t> root_signature,
                                                  std::string *why = nullptr,
                                                  std::span<const std::uint8_t> layout = {},
                                                  std::span<const std::uint8_t> records = {});
// The same, compiled for the host: empty when it is not translated or the host's compiler
// turns the translation down, and `why` says which.
std::vector<std::uint8_t> console_program_shader(ShaderStage stage,
                                                 std::span<const std::uint32_t> words,
                                                 std::span<const std::uint8_t> root_signature,
                                                 std::span<const std::uint8_t> layout = {},
                                                 std::string *why = nullptr,
                                                 std::span<const std::uint8_t> records = {});
// A geometry shader that makes the rectangles of a rectangle list: it is given the three
// corners the title draws as a triangle, and adds the fourth, opposite the first, as the
// console's GPU does. `vertex` is the pipeline's vertex shader as the host has it, whose
// outputs it passes on. Empty if none could be made.
std::span<const std::uint8_t> rectangle_shader(std::span<const std::uint8_t> vertex,
                                               bool sm50 = false);
// the inputs and outputs of a shader, by semantic, for a line of a log
std::string shader_signature(std::span<const std::uint8_t> container);
// The host's shader for it, from the cache or made now. It stays where it is for as long
// as the process runs. Empty if none could be made.
enum class HostShaderModel { sm5, sm6, automatic };
// An input of a vertex shader that the title's input layout has in the Xbox One's
// R10G10B10_SNORM_A2_UNORM, by semantic. The host reads those bits unsigned, and the shader
// made for such a layout makes the three signed again.
struct PackedInput {
    std::string name;
    unsigned index = 0;
};
// `root_signature` is the RTS0 part of the root signature the pipeline is made with, for
// a shader that carries none of its own (the later kits' shaders): what its registers hold
// is read from it, and the shader made is one for that root signature.
std::span<const std::uint8_t> host_shader(ShaderStage stage,
                                          std::span<const std::uint8_t> container,
                                          HostShaderModel model = HostShaderModel::sm5,
                                          std::span<const PackedInput> packed = {},
                                          std::span<const std::uint8_t> root_signature = {});
// XV_D3D_LOST: the start of a console shader's hash, its stage, and whether the host has
// its translation or a stand-in, and then what stopped the translation; `heaps`: of the
// shader made to read the heaps itself
std::string host_shader_note(ShaderStage stage, std::span<const std::uint8_t> container,
                             bool heaps = false);
// While on, this thread's vertex and pixel shaders of the Series console are translated
// to read their descriptors from the heaps themselves (rdna::Shader::heaps), which a
// pipeline under a root signature of the host's own takes. What such a shader's file says
// of its registers, for whoever fills them: `entries` are what the user registers hold
// (type 0 constants, 1 a root view, 3 a table; the first register, how many, the root
// parameter), `reads`
// the descriptors read (type as a range's, 3 a sampler picked by the number in register
// `reg`; else the table's register and the place in it).
void use_heap_shaders(bool on);
struct HeapLayout {
    struct Entry {
        std::uint32_t type, reg, count, slot;
    };
    struct Read {
        std::uint32_t type, reg, place;
        std::uint32_t stride = 0; // of a buffer's elements, as the program reads them
    };
    std::vector<Entry> entries;
    std::vector<Read> reads;
};
HeapLayout shader_heap_layout(std::span<const std::uint8_t> container);
// Each console shader the running title has made for the host is noted in `folder`, one file
// a shader, so that precompile_shaders makes the ones the shader cache lacks before a later
// run of the title starts. Nothing is noted while it is empty.
void set_shader_journal(std::filesystem::path folder);
// How far precompile_shaders has got, read from any thread: how many of the journal's
// entries it has looked at, then, once `scanned`, how many of those to make it has made
struct Precompile {
    std::atomic<std::size_t> journal{0}, looked{0};
    std::atomic<std::size_t> total{0}, done{0}, made{0}, failed{0};
    std::atomic<bool> scanned{false}, finished{false};
};
// Makes, in `cache`, every shader noted in `folder` that it does not have yet, on `threads`
// threads of their own that go on whatever the caller does: one the title asks for meanwhile
// is waited for, not made twice. It returns at once; the journal is looked at on a thread
// too, through an index kept beside it, so that a later start reads only the entries it has
// not seen. What does not translate, or what the host's compiler turns down, is noted so as
// not to be tried again by this version of the translator.
std::shared_ptr<Precompile> precompile_shaders(const std::filesystem::path &cache,
                                               const std::filesystem::path &folder,
                                               unsigned threads);
// A shader of a title's journal, and what the shader cache has of it by this version of the
// translator: made, noted as not made (with what stopped it), or neither yet.
struct JournalShader {
    std::filesystem::path file; // the journal's entry
    ShaderStage stage = ShaderStage::vertex;
    HostShaderModel model = HostShaderModel::sm5;
    bool heaps = false;
    std::uint32_t signed_inputs = 0;
    std::string hash, root; // hash names of the shader and of the root signature it came with
    std::uintmax_t bytes = 0;
    enum class State { made, failed, untried } state = State::untried;
    std::string why;
};
std::vector<JournalShader> journal_shaders(const std::filesystem::path &cache,
                                           const std::filesystem::path &folder);
// the HLSL made for one, as a run of the title makes it; nothing, and `why`, if it is not
std::optional<std::string> journal_shader_source(const JournalShader &shader, std::string *why);
int shader_translator_version();
// A compute shader that does nothing, for a pipeline whose own shader the host's driver
// cannot make. Empty if the host's compiler gives none.
std::span<const std::uint8_t> empty_compute_shader();
// A vertex shader that puts every vertex at the origin and a pixel shader that writes
// nothing: a pipeline of the two exists and draws nothing.
std::span<const std::uint8_t> empty_vertex_shader();
std::span<const std::uint8_t> empty_pixel_shader();
} // namespace xv::runtime::modules
