#include "Pch.h"
#include "OpenGLRenderAPI.h"
#include "OpenGLBuffer.h"
#include "OpenGLCommon.h"
#include "OpenGLShader.h"
#include "OpenGLTexture.h"
#include <Helios/VirtualDevice/DeviceTexture.h>
#include <Helios/Renderer/RenderQuery.h>
#include <glad/glad.h>

namespace Helios
{
	void OpenGLMessageCallback(
		unsigned source,
		unsigned type,
		unsigned id,
		unsigned severity,
		int length,
		const char* message,
		const void* userParam)
	{
		/* 不按消息 id 过滤，驱动性能警告是真实信号（如着色器被按 GL 状态重编译）；
		 * 消息只带 program 句柄，用登记表把 shader 名补在后面，便于定位 */
		const std::string annotated = std::string(message != nullptr ? message : "")
			+ OpenGLShader::AnnotateProgramHandlesInMessage(message);

		switch (severity)
		{
		case GL_DEBUG_SEVERITY_HIGH:         CORE_LOG_CRITICAL("{}", annotated); return;
		case GL_DEBUG_SEVERITY_MEDIUM:       CORE_LOG_ERROR("{}", annotated); return;
		case GL_DEBUG_SEVERITY_LOW:          CORE_LOG_WARN("{}", annotated); return;
		case GL_DEBUG_SEVERITY_NOTIFICATION: CORE_LOG_INFO("{}", annotated); return;
		default:
			CORE_LOG_ERROR("Unknown severity level!");
		}
	}

	void OpenGLRenderAPI::Init()
	{
		PROFILE_FUNCTION();

#ifdef HELIOS_DEBUG
#ifndef __APPLE__
		// glDebugMessageCallback requires OpenGL 4.3+, macOS only supports 4.1
		glEnable(GL_DEBUG_OUTPUT);
		glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
		glDebugMessageCallback(OpenGLMessageCallback, nullptr);
		glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION, 0, nullptr, GL_FALSE);
#endif
#endif

		// Blend
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		// Depth
		glEnable(GL_DEPTH_TEST);

		InitOpenGLExtensions();
	}

	void OpenGLRenderAPI::SetViewport(uint32_t x_start, uint32_t y_start, uint32_t width, uint32_t height)
	{
		if (!TakeViewport(x_start, y_start, width, height))
			return;

		glViewport((GLint)x_start, (GLint)y_start, (GLsizei)width, (GLsizei)height);
	}

	void OpenGLRenderAPI::SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
	{
		/* 范围等于整个 viewport 时视为全屏绘制，直接关闭裁剪（与帧缓冲绑定时一致） */
		const ViewportRegion& viewport = GetAppliedViewport();
		const bool full_screen =
			x == static_cast<uint32_t>(viewport.MinX) && y == static_cast<uint32_t>(viewport.MinY) &&
			width == viewport.Width && height == viewport.Height;

		if (!TakeScissor(x, y, width, height))
			return;

		glScissor((GLint)x, (GLint)y, (GLsizei)width, (GLsizei)height);
		if (full_screen)
			glDisable(GL_SCISSOR_TEST);
		else
			glEnable(GL_SCISSOR_TEST);
	}

	void OpenGLRenderAPI::SetClearColor(const glm::vec4& color)
	{
		glClearColor(color.r, color.g, color.b, color.a);
	}

	void OpenGLRenderAPI::Clear()
	{
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	}

	/* 应用光栅化状态：只下发变化的字段。驱动虽然也会折叠重复配置，
	 * 但每次调用仍要走一遍参数校验，逐 draw 累起来不划算。 */
	uint32_t OpenGLRenderAPI::GetStateSerial() const
	{
		return GLState::StateSerial();
	}

	void OpenGLRenderAPI::ApplyRasterState(RenderRasterState raster_state)
	{
		PROFILE_FUNCTION();

		const bool first = !m_HasAppliedRasterState;
		const RenderRasterState& applied = m_AppliedRasterState;

		/* 设置CullMode */
		if (first || applied.CullMode != raster_state.CullMode)
		{
			switch (raster_state.CullMode)
			{
			case CullMode::Cull_None:
				glDisable(GL_CULL_FACE);
				break;
			case CullMode::Cull_Front:
				glEnable(GL_CULL_FACE);
				glCullFace(GL_FRONT);
				break;
			case CullMode::Cull_Back:
				glEnable(GL_CULL_FACE);
				glCullFace(GL_BACK);
				break;
			case CullMode::Cull_Front_And_Back:
				glEnable(GL_CULL_FACE);
				glCullFace(GL_FRONT_AND_BACK);
				break;
			}
		}

		/* 设置正面顶点绕序方式 */
		if (first || applied.FrontFaceType != raster_state.FrontFaceType)
		{
			switch (raster_state.FrontFaceType)
			{
			case FrontFaceType::CW:
				glFrontFace(GL_CW);
				break;
			case FrontFaceType::CCW:
				glFrontFace(GL_CCW);
				break;
			}
		}

		/* 设置混合模式 */
		const bool blend_flag_changed = first || applied.EnableBlend != raster_state.EnableBlend;
		if (blend_flag_changed)
		{
			if (raster_state.EnableBlend)
				glEnable(GL_BLEND);
			else
				glDisable(GL_BLEND);
		}

		if (raster_state.EnableBlend &&
			(blend_flag_changed ||
			 applied.BlendEquationRGB != raster_state.BlendEquationRGB ||
			 applied.BlendEquationA != raster_state.BlendEquationA ||
			 applied.BlendFuncSrcRGB != raster_state.BlendFuncSrcRGB ||
			 applied.BlendFuncSrcA != raster_state.BlendFuncSrcA ||
			 applied.BlendFuncDstRGB != raster_state.BlendFuncDstRGB ||
			 applied.BlendFuncDstA != raster_state.BlendFuncDstA))
		{
			glBlendEquationSeparate(
				TranslateToOpenGLBlendEquation(raster_state.BlendEquationRGB),
				TranslateToOpenGLBlendEquation(raster_state.BlendEquationA));
			glBlendFuncSeparate(
				TranslateToOpenGLBlendFunc(raster_state.BlendFuncSrcRGB), 
				TranslateToOpenGLBlendFunc(raster_state.BlendFuncDstRGB), 
				TranslateToOpenGLBlendFunc(raster_state.BlendFuncSrcA), 
				TranslateToOpenGLBlendFunc(raster_state.BlendFuncDstA));
		}

		/* 设置深度测试：测试与写入分开 —— 透明物体要测遮挡但不写深度 */
		const bool depth_test_changed = first || applied.EnableDepthTest != raster_state.EnableDepthTest;
		if (depth_test_changed)
		{
			if (raster_state.EnableDepthTest)
				glEnable(GL_DEPTH_TEST);
			else
				glDisable(GL_DEPTH_TEST);
		}

		if (raster_state.EnableDepthTest)
		{
			/* 重新开测试时比较方式和写掩码可能还停在关闭前那套，一并补上 */
			if (depth_test_changed || applied.DepthCompareFunc != raster_state.DepthCompareFunc)
				glDepthFunc(TranslateToOpenGLCompareFunc(raster_state.DepthCompareFunc));
			if (depth_test_changed || applied.EnableDepthWrite != raster_state.EnableDepthWrite)
				glDepthMask(raster_state.EnableDepthWrite ? GL_TRUE : GL_FALSE);
		}

		/* 设置颜色Mask */
		if (first || applied.EnableColorWrite != raster_state.EnableColorWrite)
		{
			glColorMask((GLboolean)raster_state.EnableColorWrite, (GLboolean)raster_state.EnableColorWrite,
				(GLboolean)raster_state.EnableColorWrite, (GLboolean)raster_state.EnableColorWrite);
		}

		/* 线框填充：三角形只画边（调试线框预览用；桌面 GL 独有，GLES 没有这个状态） */
		if (first || applied.Wireframe != raster_state.Wireframe)
			glPolygonMode(GL_FRONT_AND_BACK, raster_state.Wireframe ? GL_LINE : GL_FILL);

		m_AppliedRasterState = raster_state;
		m_HasAppliedRasterState = true;
	}

	void OpenGLRenderAPI::DrawIndexed(PrimitiveType type, const SharedPtr<DeviceVertexArray>& vertex_array, uint32_t index_count, uint32_t index_offset)
	{
		PROFILE_FUNCTION();

		const uint32_t count = index_count ? index_count : vertex_array->GetVertexCount();

		/* 根据索引缓冲区的类型选择对应的GL枚举和字节偏移 */
		GLenum index_gl_type = GL_UNSIGNED_INT;
		size_t index_size = sizeof(uint32_t);
		const auto& index_buffer = vertex_array->GetIndexBuffer();
		if (index_buffer && index_buffer->GetIndexType() == IndexType::UInt16)
		{
			index_gl_type = GL_UNSIGNED_SHORT;
			index_size = sizeof(uint16_t);
		}

		glDrawElements(TranslateToOpenGLPrimitiveType(type), (GLsizei)count, index_gl_type, reinterpret_cast<const void*>(index_offset * index_size));
	}

	void OpenGLRenderAPI::DrawArrays(PrimitiveType type, const SharedPtr<DeviceVertexArray>& vertex_array)
	{
		PROFILE_FUNCTION();

		const auto count = vertex_array->GetVertexCount();
		glDrawArrays(TranslateToOpenGLPrimitiveType(type), 0, count);
	}

	void OpenGLRenderAPI::BindInstanceAttributes(const SharedPtr<DeviceVertexArray>& vertex_array,
		const SharedPtr<DeviceVertexBuffer>& instance_buffer, uint32_t instance_offset_bytes,
		const VertexBufferLayout& instance_layout)
	{
		if (instance_buffer == nullptr || instance_layout.GetElements().empty())
			return;

		/* 属性指针存在 VAO 里：换批次时按新偏移重挂一遍，成本按批次摊 */
		vertex_array->Bind();
		instance_buffer->Bind();

		const VertexBufferLayout& layout = instance_layout;
		const uint32_t stride = layout.GetStride();
		const auto base_offset = static_cast<size_t>(instance_offset_bytes);

		uint32_t location = 5;
		for (const BufferElement& element : layout.GetElements())
		{
			glEnableVertexAttribArray(location);
			glVertexAttribPointer(location, static_cast<GLint>(element.GetComponentCount()),
				TranslateToOpenGLBaseType(element.Type),
				element.Normalized ? GL_TRUE : GL_FALSE, static_cast<GLsizei>(stride),
				reinterpret_cast<const void*>(base_offset + element.Offset));
			glVertexAttribDivisor(location, 1);
			++location;
		}
	}

	void OpenGLRenderAPI::DrawArraysInstanced(PrimitiveType type, const SharedPtr<DeviceVertexArray>& vertex_array,
		const SharedPtr<DeviceVertexBuffer>& instance_buffer, uint32_t instance_offset_bytes,
		uint32_t instance_count, const VertexBufferLayout& instance_layout)
	{
		PROFILE_FUNCTION();

		BindInstanceAttributes(vertex_array, instance_buffer, instance_offset_bytes, instance_layout);
		glDrawArraysInstanced(TranslateToOpenGLPrimitiveType(type), 0,
			static_cast<GLsizei>(vertex_array->GetVertexCount()), static_cast<GLsizei>(instance_count));
	}

	void OpenGLRenderAPI::DrawIndexedInstanced(PrimitiveType type, const SharedPtr<DeviceVertexArray>& vertex_array,
		const SharedPtr<DeviceVertexBuffer>& instance_buffer, uint32_t instance_offset_bytes,
		uint32_t instance_count, const VertexBufferLayout& instance_layout,
		uint32_t index_count, uint32_t index_offset)
	{
		PROFILE_FUNCTION();

		BindInstanceAttributes(vertex_array, instance_buffer, instance_offset_bytes, instance_layout);

		const auto index_buffer = vertex_array->GetIndexBuffer();
		const uint32_t count = index_count > 0 ? index_count
			: (index_buffer != nullptr ? index_buffer->GetCount() : 0);
		const auto index_type = index_buffer != nullptr && index_buffer->GetIndexType() == IndexType::UInt16
			? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
		const auto index_size = index_type == GL_UNSIGNED_SHORT ? sizeof(uint16_t) : sizeof(uint32_t);

		glDrawElementsInstanced(TranslateToOpenGLPrimitiveType(type), static_cast<GLsizei>(count), index_type,
			reinterpret_cast<const void*>(static_cast<size_t>(index_offset) * index_size),
			static_cast<GLsizei>(instance_count));
	}

	bool OpenGLRenderAPI::DispatchCompute(const SharedPtr<DeviceShader>& shader,
		const std::vector<std::pair<uint32_t, SharedPtr<DeviceStorageBuffer>>>& buffers,
		uint32_t param_binding, const void* param_data, uint32_t param_size,
		uint32_t group_count)
	{
#ifdef __APPLE__
		/* macOS 的 GL 只有 4.1，没有计算着色器 */
		(void)shader;
		(void)buffers;
		(void)param_binding;
		(void)param_data;
		(void)param_size;
		(void)group_count;
		return false;
#else
		PROFILE_FUNCTION();

		auto* gl_shader = dynamic_cast<OpenGLShader*>(shader.get());
		if (gl_shader == nullptr || !gl_shader->IsCompute() || group_count == 0)
			return false;

		/* 先把缓冲全部解析出来：半路失败不会留下改了一半的绑定 */
		struct ResolvedBinding
		{
			GLenum Target;
			GLuint Binding;
			GLuint Buffer;
		};
		const auto& uniform_blocks = shader->GetReflectionData().UniformBlockBindings;
		std::vector<ResolvedBinding> resolved;
		resolved.reserve(buffers.size());
		for (const auto& [binding, buffer] : buffers)
		{
			auto gl_buffer = std::dynamic_pointer_cast<OpenGLStorageBuffer>(buffer);
			if (gl_buffer == nullptr || gl_buffer->GetBufferID() == 0)
				return false;

			/* uniform 块要挂到 GL_UNIFORM_BUFFER，其余按 SSBO 走 */
			const GLenum target = uniform_blocks.find(binding) != uniform_blocks.end()
				? GL_UNIFORM_BUFFER : GL_SHADER_STORAGE_BUFFER;
			resolved.push_back({ target, binding, gl_buffer->GetBufferID() });
		}

		gl_shader->Bind();

		/* binding point 是上下文级状态，引擎的 uniform 缓冲靠它常驻。派发临时占用几个点，
		 * 先从后端记录里取出原值，派发完原样还回去（不查驱动） */
		struct SavedBinding
		{
			GLenum Target;
			GLuint Binding;
			GLState::BufferBinding Range;
		};
		std::vector<SavedBinding> saved;
		saved.reserve(resolved.size() + 1);
		for (const ResolvedBinding& item : resolved)
		{
			saved.push_back({ item.Target, item.Binding, GLState::CurrentBufferRange(item.Target, item.Binding) });
			GLState::BindBufferRange(item.Target, item.Binding, { item.Buffer, 0, 0 });
		}

		/* 这一趟的常量块：拷进临时 UBO，按 std140 最小 16 字节挂到指定 binding */
		if (param_data != nullptr && param_size > 0)
		{
			const uint32_t block_size = std::max<uint32_t>(16u, (param_size + 15u) / 16u * 16u);
			if (m_ComputeParamBuffer == 0)
			{
				GLuint buffer = 0;
				glCreateBuffers(1, &buffer);
				m_ComputeParamBuffer = buffer;
			}
			if (m_ComputeParamBufferSize < block_size)
			{
				glNamedBufferData(m_ComputeParamBuffer, block_size, nullptr, GL_DYNAMIC_DRAW);
				m_ComputeParamBufferSize = block_size;
			}
			glNamedBufferSubData(m_ComputeParamBuffer, 0, param_size, param_data);

			saved.push_back({ GL_UNIFORM_BUFFER, param_binding, GLState::CurrentBufferRange(GL_UNIFORM_BUFFER, param_binding) });
			GLState::BindBufferRange(GL_UNIFORM_BUFFER, param_binding, { m_ComputeParamBuffer, 0, block_size });
		}

		glDispatchCompute(group_count, 1, 1);
		/* 结果要给后续的顶点拉取 / 存储缓冲读写用，等写入可见再往下走 */
		glMemoryBarrier(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

		for (auto it = saved.rbegin(); it != saved.rend(); ++it)
			GLState::BindBufferRange(it->Target, it->Binding, it->Range);

		return true;
#endif
	}

	/* 单上下文按顺序执行，Pass 之间不用提交；交换缓冲时驱动自己会 flush */
	void OpenGLRenderAPI::Flush()
	{
	}

	void OpenGLRenderAPI::GenerateMipmap(const SharedPtr<DeviceTexture>& texture)
	{
		PROFILE_FUNCTION();

		/* OpenGL 的命令是立即执行的，烘焙后的绘制已经生效，直接生成即可 */
		if (texture)
			texture->GenerateMipmap();
	}

	SharedPtr<DeviceTexture> OpenGLRenderAPI::AcquireDefaultTargetSnapshot(bool& top_down)
	{
		PROFILE_FUNCTION();

		/* 默认目标的区域 = 当前视口（后端记着，不用查驱动）：渲染到默认目标的 Pass 已把视口
		 * 设成它的目标区域。没设过视口或尺寸为 0 视为无可用目标、静默跳过 */
		const ViewportRegion& viewport = GetAppliedViewport();
		if (viewport.Width == 0 || viewport.Height == 0)
			return nullptr;

		const uint32_t width = viewport.Width;
		const uint32_t height = viewport.Height;

		/* 预览纹理跨帧复用；尺寸变化（窗口缩放）时重建 */
		if (m_DefaultTargetSnapshot == nullptr
			|| m_DefaultTargetSnapshot->GetTextureDesc().Width != width
			|| m_DefaultTargetSnapshot->GetTextureDesc().Height != height)
		{
			TextureDesc desc;
			desc.Width = width;
			desc.Height = height;
			desc.MipLevels = 1;
			desc.Samples = 1;
			desc.Format = TextureFormat::RGBA8;
			desc.SamplerType = SamplerType::Sampler2D;
			desc.Usage = TextureUsage::Sampleable;
			m_DefaultTargetSnapshot = DeviceTexture::Create("DefaultTargetSnapshot", desc);
		}
		const auto snapshot = std::dynamic_pointer_cast<OpenGLTexture>(m_DefaultTargetSnapshot);
		if (snapshot == nullptr)
			return nullptr;

		/* 在 GPU 上直接从默认帧缓冲 blit 到快照纹理，不经过 CPU；行序同 glReadPixels（底行在前） */
		const GLuint previous_framebuffer = GLState::CurrentFramebuffer();
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, AcquireCopyFramebuffer(1));
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, snapshot->GetTextureID(), 0);
		glDrawBuffer(GL_COLOR_ATTACHMENT0);
		glReadBuffer(GL_BACK);
		glBlitFramebuffer(viewport.MinX, viewport.MinY, viewport.MinX + static_cast<GLint>(width), viewport.MinY + static_cast<GLint>(height),
			0, 0, static_cast<GLint>(width), static_cast<GLint>(height), GL_COLOR_BUFFER_BIT, GL_NEAREST);
		GLState::BindFramebuffer(previous_framebuffer);

		top_down = false;
		return m_DefaultTargetSnapshot;
	}

	/* 把源纹理 mip 0 的第 src_layer 层拷到目标纹理（逐 Pass 抓取 Attachment → 常驻预览）。
	 * 走"临时 FBO + glBlitFramebuffer"：不做 4.3+ 的 glCopyImageSubData 依赖（同一份代码
	 * 要在 4.1 上下文上可用，理由与 ClearAttachment 的 __APPLE__ 分支相同）。源挂读 FBO
	 * （2D 数组用 glFramebufferTextureLayer 指定层）、目标挂画 FBO，同尺寸同格式直拷；
	 * 深度格式按 GL_DEPTH_BUFFER_BIT 走（规范要求 NEAREST），且两个 FBO 的读写缓冲都置
	 * NONE（"只有深度附件"的 FBO 在部分驱动上要这样才判定完整）。本函数在 Pass 中间被
	 * 调用，调用点的 FBO 绑定先保存、结束后还原 —— 不能把别人的绑定改掉。 */
	void OpenGLRenderAPI::CopyTexture(const SharedPtr<DeviceTexture>& src, const SharedPtr<DeviceTexture>& dst,
		uint32_t src_layer)
	{
		PROFILE_FUNCTION();

		const auto open_gl_src = std::dynamic_pointer_cast<OpenGLTexture>(src);
		const auto open_gl_dst = std::dynamic_pointer_cast<OpenGLTexture>(dst);
		if (!open_gl_src || !open_gl_dst || open_gl_src->GetTextureID() == 0 || open_gl_dst->GetTextureID() == 0)
			return;

		const TextureDesc& src_desc = src->GetTextureDesc();
		if (src_layer >= std::max(1u, src_desc.Depth))
		{
			CORE_LOG_ERROR("OpenGLRenderAPI::CopyTexture: layer {} out of range ({})",
				src_layer, src_desc.Depth);
			return;
		}

		const bool is_depth = IsDepthFormat(src_desc.Format);
		const GLenum attachment = is_depth ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0;
		const GLbitfield blit_mask = is_depth ? GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT;

		/* 两个拷贝用 FBO 常驻复用；调用点的绑定从后端记录里取，拷完原样还回去 */
		const GLuint previous_framebuffer = GLState::CurrentFramebuffer();
		const GLuint read_fbo = AcquireCopyFramebuffer(0);
		const GLuint draw_fbo = AcquireCopyFramebuffer(1);

		glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);

		/* 源和目标都可能是纹理或 RenderBuffer；纹理再按采样类型挂到 2D、单层或立方体面 */
		if (open_gl_src->IsRenderBuffer())
		{
			glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, attachment, GL_RENDERBUFFER, open_gl_src->GetTextureID());
		}
		else
		{
			switch (src_desc.SamplerType)
			{
			case SamplerType::Sampler2DArray:
			case SamplerType::Sampler3D:
				glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, attachment, open_gl_src->GetTextureID(), 0, static_cast<GLint>(src_layer));
				break;
			case SamplerType::SamplerCubeMap:
				glFramebufferTexture2D(GL_READ_FRAMEBUFFER, attachment, GL_TEXTURE_CUBE_MAP_POSITIVE_X + src_layer, open_gl_src->GetTextureID(), 0);
				break;
			default:
				glFramebufferTexture2D(GL_READ_FRAMEBUFFER, attachment, GL_TEXTURE_2D, open_gl_src->GetTextureID(), 0);
				break;
			}
		}

		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
		if (open_gl_dst->IsRenderBuffer())
		{
			glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, attachment, GL_RENDERBUFFER, open_gl_dst->GetTextureID());
		}
		else
		{
			glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment, GL_TEXTURE_2D, open_gl_dst->GetTextureID(), 0);
		}

		if (is_depth)
		{
			glReadBuffer(GL_NONE);
			glDrawBuffer(GL_NONE);
		}
		else
		{
			glReadBuffer(GL_COLOR_ATTACHMENT0);
			glDrawBuffer(GL_COLOR_ATTACHMENT0);
		}

		const GLenum read_status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
		const GLenum draw_status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
		if (read_status == GL_FRAMEBUFFER_COMPLETE && draw_status == GL_FRAMEBUFFER_COMPLETE)
		{
			glBlitFramebuffer(0, 0, static_cast<GLint>(src_desc.Width), static_cast<GLint>(src_desc.Height),
				0, 0, static_cast<GLint>(src_desc.Width), static_cast<GLint>(src_desc.Height),
				blit_mask, GL_NEAREST);
		}
		else
		{
			/* 附件不完整时把源资源信息一并打出，便于定位是哪路资源 */
			CORE_LOG_ERROR("OpenGLRenderAPI::CopyTexture: framebuffer incomplete (read 0x{:X}, draw 0x{:X})"
				" src='{}' ({}x{}, sampler {}, format {}, mips {}, layer {})",
				read_status, draw_status,
				src->GetDebugName(), src_desc.Width, src_desc.Height,
				static_cast<int>(src_desc.SamplerType), static_cast<int>(src_desc.Format),
				src_desc.MipLevels, src_layer);
		}

		/* 附件摘掉，免得常驻 FBO 一直引用着已经释放的纹理 */
		glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, attachment, GL_RENDERBUFFER, 0);
		glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, attachment, GL_RENDERBUFFER, 0);
		GLState::BindFramebuffer(previous_framebuffer);

		CHECK_GL_ERROR;
	}

	/* 拷贝 / 快照用的两个 FBO（0 = 读、1 = 写），第一次用时建，之后常驻 */
	GLuint OpenGLRenderAPI::AcquireCopyFramebuffer(uint32_t index)
	{
		if (m_CopyFramebuffers[index] == 0)
			glGenFramebuffers(1, &m_CopyFramebuffers[index]);
		return m_CopyFramebuffers[index];
	}

	/* Debug 组同时驱动 GPU 耗时作用域：组名是抓帧分组与统计面板共用的唯一标签源。
	 * 作用域无条件配对，不依赖 KHR_debug 是否可用（作用域树与抓帧组解耦）。 */
	void OpenGLRenderAPI::PushDebugGroup(const char* name)
	{
		PROFILE_FUNCTION();

		if (name)
			RenderQueryProfiler::Instance().BeginGPUScope(name);

		if (m_SupportedExtensions.KHR_debug)
			glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, name);
	}

	void OpenGLRenderAPI::PopDebugGroup()
	{
		PROFILE_FUNCTION();

		RenderQueryProfiler::Instance().EndGPUScope();

		if (m_SupportedExtensions.KHR_debug)
			glPopDebugGroup();
	}

	/* 获取OpenGL支持的扩展 */
	void OpenGLRenderAPI::InitOpenGLExtensions()
	{
		PROFILE_FUNCTION();

		/* 获取扩展数 */
		GLint num = 0;
		glGetIntegerv(GL_NUM_EXTENSIONS, &num);
		/* 收集扩展 */
		std::unordered_set<std::string_view> extensions;
		for (auto i = 0; i < num; ++i)
			extensions.emplace((const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i));

#ifdef HELIOS_DEBUG
		CORE_LOG_INFO("Supported Extensions:");
		for (auto& ext : extensions)
			CORE_LOG_INFO("- {}", ext);
#endif

		m_SupportedExtensions.KHR_debug = extensions.find(std::string_view("GL_KHR_debug")) != extensions.end();
		m_SupportedExtensions.EXT_shader_framebuffer_fetch = extensions.find(std::string_view("GL_EXT_shader_framebuffer_fetch")) != extensions.end();
		m_SupportedExtensions.EXT_texture_filter_anisotropic = extensions.find(std::string_view("GL_EXT_texture_filter_anisotropic")) != extensions.end();
		m_SupportedExtensions.EXT_texture_sRGB = extensions.find(std::string_view("GL_EXT_texture_sRGB")) != extensions.end();
	}
}
