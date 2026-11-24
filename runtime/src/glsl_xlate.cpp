/*
 * glsl_xlate.cpp - desktop GLSL (the game's #version 150 core shaders) ->
 * GLSL ES 3.10 for OpenGL ES hosts.
 *
 * GLSL ES has no implicit int->float conversions and other strictness the
 * game's shaders rely on, so the source is compiled for real: glslang parses it
 * as desktop GLSL and emits SPIR-V, SPIRV-Cross writes ESSL from that. Explicit
 * locations the round trip invents are removed again, so attributes keep
 * following glBindAttribLocation, uniforms are found by name and stage
 * interfaces match by name, exactly as with the original source.
 */
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>
#include <spirv_glsl.hpp>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern "C" void w64_log(const char *fmt, ...);

static std::once_flag g_init;

static EShLanguage stage_of(const std::string &src, int gl_type)
{
    if (gl_type == 0x8B31) return EShLangVertex;     /* GL_VERTEX_SHADER */
    if (gl_type == 0x8B30) return EShLangFragment;   /* GL_FRAGMENT_SHADER */
    if (gl_type == 0x8DD9) return EShLangGeometry;   /* GL_GEOMETRY_SHADER */
    (void)src;
    return EShLangVertex;
}

extern "C" char *w64_glsl_to_essl(const char *src_in, int gl_type, int es_version)
{
    std::call_once(g_init, [] { glslang::InitializeProcess(); });
    std::string src(src_in);
    /* glslang only emits OpenGL SPIR-V from #version 330 on; 150 core -> 330 core
     * changes nothing the game's shaders use. */
    size_t v = src.find("#version");
    if (v != std::string::npos) {
        size_t e = src.find('\n', v);
        src.replace(v, e - v, "#version 330 core");
    } else {
        src = "#version 330 core\n" + src;
    }
    EShLanguage st = stage_of(src, gl_type);
    glslang::TShader sh(st);
    const char *strs[] = { src.c_str() };
    sh.setStrings(strs, 1);
    sh.setEnvInput(glslang::EShSourceGlsl, st, glslang::EShClientOpenGL, 100);
    sh.setEnvClient(glslang::EShClientOpenGL, glslang::EShTargetOpenGL_450);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    sh.setAutoMapLocations(true);
    sh.setAutoMapBindings(true);
    EShMessages msg = (EShMessages)(EShMsgDefault | EShMsgSpvRules);
    if (!sh.parse(GetDefaultResources(), 330, ECoreProfile, false, false, msg)) {
        w64_log("[glsl] parse failed: %s", sh.getInfoLog());
        return nullptr;
    }
    glslang::TProgram prog;
    prog.addShader(&sh);
    if (!prog.link(msg) || !prog.mapIO()) {
        w64_log("[glsl] link failed: %s", prog.getInfoLog());
        return nullptr;
    }
    std::vector<uint32_t> spv;
    glslang::SpvOptions so;
    so.generateDebugInfo = false;
    so.disableOptimizer = true;
    glslang::GlslangToSpv(*prog.getIntermediate(st), spv, &so);

    try {
        spirv_cross::CompilerGLSL x(std::move(spv));
        spirv_cross::ShaderResources r = x.get_shader_resources();
        auto strip = [&](const spirv_cross::SmallVector<spirv_cross::Resource> &list, bool loc, bool bind) {
            for (auto &res : list) {
                if (loc) x.unset_decoration(res.id, spv::DecorationLocation);
                if (bind) x.unset_decoration(res.id, spv::DecorationBinding);
            }
        };
        strip(r.gl_plain_uniforms, true, true);
        strip(r.sampled_images, true, true);
        strip(r.separate_images, true, true);
        strip(r.uniform_buffers, false, false);
        if (st == EShLangVertex) strip(r.stage_inputs, true, false);
        if (st != EShLangFragment) strip(r.stage_outputs, true, false);
        if (st != EShLangVertex) strip(r.stage_inputs, true, false);
        if (st == EShLangFragment && r.stage_outputs.size() <= 1) strip(r.stage_outputs, true, false);
        spirv_cross::CompilerGLSL::Options o;
        o.es = true;
        o.version = (uint32_t)es_version;
        o.fragment.default_float_precision = spirv_cross::CompilerGLSL::Options::Highp;
        o.fragment.default_int_precision = spirv_cross::CompilerGLSL::Options::Highp;
        o.enable_420pack_extension = false;
        o.emit_uniform_buffer_as_plain_uniforms = false;
        x.set_common_options(o);
        std::string out = x.compile();
        char *ret = (char *)malloc(out.size() + 1);
        memcpy(ret, out.c_str(), out.size() + 1);
        return ret;
    } catch (const std::exception &e) {
        w64_log("[glsl] SPIRV-Cross failed: %s", e.what());
        return nullptr;
    }
}
