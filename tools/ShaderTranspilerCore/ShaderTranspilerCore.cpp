#include "ShaderTranspilerCore.h"

#include <glslang/Include/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <iostream>
#include <spirv_cross/spirv_cross.hpp>
#include <spirv_cross/spirv_glsl.hpp>
#include <spirv_cross/spirv_hlsl.hpp>
#include <spirv_cross/spirv_msl.hpp>
#include <spirv_cross/spirv_reflect.hpp>

#include "ResourceLimits.h"

namespace LLGI
{

EShLanguage GetGlslangShaderStage(ShaderStageType type)
{
	if (type == ShaderStageType::Vertex)
		return EShLanguage::EShLangVertex;
	if (type == ShaderStageType::Pixel)
		return EShLanguage::EShLangFragment;
	throw std::string("Unimplemented ShaderStage");
}

SPIRV::SPIRV(const std::vector<uint32_t>& data, ShaderStageType shaderStage) : data_(data), shaderStage_(shaderStage) {}

SPIRV::SPIRV(const std::string& error) : error_(error), shaderStage_{} {}

ShaderStageType SPIRV::GetStage() const { return shaderStage_; }

const std::vector<uint32_t>& SPIRV::GetData() const { return data_; }

bool SPIRVTranspiler::Transpile(const std::shared_ptr<SPIRV>& spirv) { return false; }

std::string SPIRVTranspiler::GetErrorCode() const { return errorCode_; }

std::string SPIRVTranspiler::GetCode() const { return code_; }

bool SPIRVToHLSLTranspiler::Transpile(const std::shared_ptr<SPIRV>& spirv)
{
	spirv_cross::CompilerHLSL compiler(spirv->GetData());

	spirv_cross::CompilerGLSL::Options options;
	options.separate_shader_objects = true;
	compiler.set_common_options(options);

	spirv_cross::CompilerHLSL::Options targetOptions;
	targetOptions.shader_model = 40;
	compiler.set_hlsl_options(targetOptions);

	code_ = compiler.compile();

	return true;
}

bool SPIRVToMSLTranspiler::Transpile(const std::shared_ptr<SPIRV>& spirv)
{
	spirv_cross::CompilerMSL compiler(spirv->GetData());

	spirv_cross::ShaderResources resources = compiler.get_shader_resources();

	spirv_cross::CompilerGLSL::Options options;
	compiler.set_common_options(options);

	spirv_cross::CompilerMSL::Options targetOptions;
	compiler.set_msl_options(targetOptions);

	code_ = compiler.compile();

	return true;
}

bool SPIRVToGLSLTranspiler::Transpile(const std::shared_ptr<SPIRV>& spirv)
{
	spirv_cross::CompilerGLSL compiler(spirv->GetData());

	// to combine a sampler and a texture
	compiler.build_combined_image_samplers();

	spirv_cross::ShaderResources resources = compiler.get_shader_resources();

	int32_t binding_offset = 0;

	if (isVulkanMode_)
	{
		binding_offset += 1;
	}

	for (auto& resource : resources.sampled_images)
	{
		auto i = compiler.get_decoration(resource.id, spv::DecorationLocation);
		compiler.set_decoration(resource.id, spv::DecorationBinding, binding_offset + i);

		if (spirv->GetStage() == ShaderStageType::Vertex)
		{
			if (isVulkanMode_)
			{
				compiler.set_decoration(resource.id, spv::DecorationDescriptorSet, 0);
			}
		}
		else if (spirv->GetStage() == ShaderStageType::Pixel)
		{
			if (isVulkanMode_)
			{
				compiler.set_decoration(resource.id, spv::DecorationDescriptorSet, 1);
			}
		}
	}

	for (auto& resource : resources.uniform_buffers)
	{
		if (spirv->GetStage() == ShaderStageType::Vertex)
		{
			if (isVulkanMode_)
			{
				compiler.set_decoration(resource.id, spv::DecorationBinding, 0);
				compiler.set_decoration(resource.id, spv::DecorationDescriptorSet, 0);
			}
		}
		else if (spirv->GetStage() == ShaderStageType::Pixel)
		{
			if (isVulkanMode_)
			{
				compiler.set_decoration(resource.id, spv::DecorationBinding, 0);
				compiler.set_decoration(resource.id, spv::DecorationDescriptorSet, 1);
			}
		}
	}

	spirv_cross::CompilerGLSL::Options options;
	options.version = 420;
	options.enable_420pack_extension = true;
	options.vulkan_semantics = isVulkanMode_;
	compiler.set_common_options(options);

	code_ = compiler.compile();

	return true;
}

class ReflectionCompiler : public spirv_cross::Compiler
{
public:
	ReflectionCompiler(const std::vector<uint32_t>& data) : Compiler(data) {}
	virtual ~ReflectionCompiler() = default;

	size_t get_member_count(uint32_t id) const
	{
		const spirv_cross::Meta& m = ir.meta.at(id);
		return m.members.size();
	}

	spirv_cross::SPIRType get_member_type(const spirv_cross::SPIRType& struct_type, uint32_t index) const
	{
		return get<spirv_cross::SPIRType>(struct_type.member_types[index]);
	}
};

bool SPIRVReflection::Transpile(const std::shared_ptr<SPIRV>& spirv)
{
	Textures.clear();
	Uniforms.clear();

	ReflectionCompiler compiler(spirv->GetData());
	spirv_cross::ShaderResources resources = compiler.get_shader_resources();

	// Texture
	for (const auto& sampler : resources.separate_images)
	{
		ShaderReflectionTexture t;
		t.Name = sampler.name;
		t.Offset = compiler.get_decoration(sampler.id, spv::DecorationBinding);
		Textures.push_back(t);
	}

	// Uniform
	for (const auto& resource : resources.uniform_buffers)
	{
		auto count = compiler.get_member_count(resource.base_type_id);
		auto spirvType = compiler.get_type(resource.type_id);

		for (auto i = 0; i < count; i++)
		{
			ShaderReflectionUniform u;
			auto memberType = compiler.get_member_type(spirvType, i);
			u.Name = compiler.get_member_name(resource.base_type_id, i);
			u.Size = static_cast<int32_t>(compiler.get_declared_struct_member_size(spirvType, i));
			u.Offset = compiler.get_member_decoration(resource.base_type_id, i, spv::DecorationOffset);
			Uniforms.push_back(u);
		}
	}

	return true;
}

SPIRVGenerator::SPIRVGenerator() { glslang::InitializeProcess(); }

SPIRVGenerator::~SPIRVGenerator() { glslang::FinalizeProcess(); }

std::shared_ptr<SPIRV> SPIRVGenerator::Generate(const char* code, ShaderStageType shaderStageType, bool isYInverted)
{
	std::string codeStr(code);
	glslang::TProgram program;
	TBuiltInResource resources = glslang::DefaultTBuiltInResource;
	auto shaderStage = GetGlslangShaderStage(shaderStageType);

	glslang::TShader shader = glslang::TShader(shaderStage);
	shader.setEnvInput(glslang::EShSourceHlsl, shaderStage, glslang::EShClientOpenGL, glslang::EShTargetOpenGL_450);
	shader.setEnvClient(glslang::EShClientOpenGL, glslang::EShTargetOpenGL_450);
	shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

	if (isYInverted)
	{
		shader.setInvertY(true);
	}

	const char* shaderStrings[1];
	shaderStrings[0] = codeStr.c_str();
	shader.setEntryPoint("main");
	// shader->setAutoMapBindings(true);
	// shader->setAutoMapLocations(true);

	shader.setStrings(shaderStrings, 1);
	EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
	messages = (EShMessages)(messages | EShMsgReadHlsl);
	messages = (EShMessages)(messages | EShOptFull);

	int defaultVersion = 110;
	if (!shader.parse(&resources, defaultVersion, false, messages))
	{
		return std::make_shared<SPIRV>(shader.getInfoLog());
	}

	program.addShader(&shader);

	if (!program.link(messages))
	{
		return std::make_shared<SPIRV>(program.getInfoLog());
	}

	std::vector<unsigned int> spirv;
	glslang::SpvOptions spvOptions;
	spvOptions.optimizeSize = true;
	spvOptions.disableOptimizer = false;

	glslang::GlslangToSpv(*program.getIntermediate(shaderStage), spirv, &spvOptions);

	return std::make_shared<SPIRV>(spirv, shaderStageType);
}

} // namespace LLGI
