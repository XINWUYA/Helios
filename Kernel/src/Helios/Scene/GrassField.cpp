#include "Pch.h"
#include "GrassField.h"
#include "TerrainField.h"
#include <glm/gtc/type_ptr.hpp>
#include "Components.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/VirtualDevice/DeviceTexture.h"
#include "Helios/VirtualDevice/DeviceVertexArray.h"

namespace Helios
{
	namespace
	{
		/* 块边长：与剔除粒度一致，8 米一块在三十万株时大概几百块 */
		constexpr float kChunkSize = 8.0f;
		/* 单块草地的实例上限：参数填大了不至于把显存吃光 */
		constexpr uint32_t kMaxInstances = 2u * 1024u * 1024u;
		/* 脚印图分辨率：256 见方铺满整片草地。128 时一格将近半米，
		 * 一个 1.8 米的影响半径只盖住四五个格子，压下去看不出形状 */
		constexpr uint32_t kTrampleSize = 256;

		bool g_GpuPlacementEnabled = false;
		bool g_IndirectDrawEnabled = false;

		/* 叶片模板的索引数：间接参数里的 count */
		constexpr uint32_t kBladeIndexCount = 24;

		/* PCG 的输出散列：雪崩够彻底。早先用的是异或加移位那套，
		 * 相邻序号只差低位，散出来是渐变的，叶片会排成一条条直线。 */
		uint32_t MixHash(uint32_t value)
		{
			value = value * 747796405u + 2891336453u;
			const uint32_t word = ((value >> ((value >> 28u) + 4u)) ^ value) * 277803737u;
			return (word >> 22u) ^ word;
		}

		/* 同一株草的随机通道：基值不同、通道号再散一次，各属性才互不相关 */
		float HashUnit(uint32_t base, uint32_t channel)
		{
			return static_cast<float>(MixHash(base + channel * 0x9e3779b9u) & 0xFFFFFFu)
				/ static_cast<float>(0x1000000u);
		}

		constexpr float kTwoPi = 6.2831853f;
	}

	void GrassField::SetGlobalGpuPlacementEnabled(bool enabled)
	{
		g_GpuPlacementEnabled = enabled;
	}

	bool GrassField::IsGlobalGpuPlacementEnabled()
	{
		return g_GpuPlacementEnabled;
	}

	void GrassField::SetGlobalIndirectDrawEnabled(bool enabled)
	{
		g_IndirectDrawEnabled = enabled;
	}

	bool GrassField::IsGlobalIndirectDrawEnabled()
	{
		return g_IndirectDrawEnabled;
	}

	void GrassField::Rebuild(const GrassComponent& component, const glm::mat4& world,
		const TerrainField* ground)
	{
		PROFILE_FUNCTION();

		/* 构建键：参数、变换、放置方式都算进来，没变就整段跳过 */
		uint64_t key = 0xcbf29ce484222325ull;
		const auto mix = [&key](const void* data, size_t size)
		{
			const auto* bytes = static_cast<const uint8_t*>(data);
			for (size_t i = 0; i < size; ++i)
			{
				key ^= bytes[i];
				key *= 0x100000001b3ull;
			}
		};
		mix(&component.m_Extent, sizeof(component.m_Extent));
		for (const float value : { component.m_Density, component.m_Height, component.m_Width,
			component.m_LodDistance, component.m_CullDistance, component.m_MinDensityScale })
		{
			mix(&value, sizeof(value));
		}
		mix(&component.m_Seed, sizeof(component.m_Seed));
		mix(&component.m_SurfaceAlign, sizeof(component.m_SurfaceAlign));
		mix(&component.m_MaxSlope, sizeof(component.m_MaxSlope));
		mix(&g_GpuPlacementEnabled, sizeof(g_GpuPlacementEnabled));
		mix(glm::value_ptr(world), sizeof(glm::mat4));
		/* 地形换了一份或重建过：贴地高度要重新采样 */
		const uint64_t ground_key = ground != nullptr ? ground->GetRevision() : 0ull;
		mix(&ground_key, sizeof(ground_key));

		if (key == m_BuildKey && m_InstanceBuffer != nullptr)
			return;
		m_BuildKey = key;
		m_Ground = ground;

		m_Chunks.clear();
		m_InstanceCount = 0;
		m_GpuDirty = false;
		/* 实例数变了，压实缓冲的大小要重来 */
		m_IndirectDrawBuffer = nullptr;
		m_IndirectDrawView = nullptr;

		/* 区域按世界尺度展开：组件的范围是局部空间的，缩放挂在实体变换上 */
		const glm::vec3 axis_x(world[0]);
		const glm::vec3 axis_y(world[1]);
		const glm::vec3 axis_z(world[2]);
		m_ScaleX = glm::length(axis_x);
		m_ScaleY = glm::length(axis_y);
		const float scale_z = glm::length(axis_z);
		m_Right = m_ScaleX > 1e-5f ? axis_x / m_ScaleX : glm::vec3(1.0f, 0.0f, 0.0f);
		m_Forward = scale_z > 1e-5f ? axis_z / scale_z : glm::vec3(0.0f, 0.0f, 1.0f);
		m_Origin = glm::vec3(world[3]);

		m_HalfX = std::max(0.0f, component.m_Extent.x * m_ScaleX);
		m_HalfZ = std::max(0.0f, component.m_Extent.y * scale_z);
		const float density = std::max(0.0f, component.m_Density);
		m_Height = std::max(0.0f, component.m_Height);
		m_Width = std::max(0.0f, component.m_Width);
		m_SurfaceAlign = std::clamp(component.m_SurfaceAlign, 0.0f, 1.0f);
		m_MaxSlope = std::max(0.0f, component.m_MaxSlope);
		m_LodDistance = component.m_LodDistance;
		m_CullDistance = component.m_CullDistance;
		m_MinDensityScale = component.m_MinDensityScale;
		m_ReceiveShadow = component.m_ReceiveShadow;
		m_Seed = static_cast<uint32_t>(std::max(0, component.m_Seed));

		if (m_HalfX <= 0.0f || m_HalfZ <= 0.0f || density <= 0.0f || m_Height <= 0.0f)
		{
			m_InstanceBuffer = nullptr;
			m_InstanceStorage = nullptr;
			m_GpuVertexView = nullptr;
			m_PadBuffer = nullptr;
			m_TrampleTexture = nullptr;
			m_Trample.clear();
			m_CapacityBytes = 0;
			return;
		}

		m_ChunksX = std::max(1u, static_cast<uint32_t>(std::ceil(m_HalfX * 2.0f / kChunkSize)));
		m_ChunksZ = std::max(1u, static_cast<uint32_t>(std::ceil(m_HalfZ * 2.0f / kChunkSize)));
		m_ChunkWidth = m_HalfX * 2.0f / static_cast<float>(m_ChunksX);
		m_ChunkDepth = m_HalfZ * 2.0f / static_cast<float>(m_ChunksZ);

		/* 每块株数固定：GPU 侧才能用「总序号 ÷ 每块株数」反推块号，不用查表 */
		uint32_t per_chunk = static_cast<uint32_t>(
			std::max(0.0f, std::round(density * m_ChunkWidth * m_ChunkDepth)));
		const uint32_t chunk_count = m_ChunksX * m_ChunksZ;
		while (per_chunk > 0 && per_chunk * chunk_count > kMaxInstances)
			--per_chunk;
		m_PerChunk = per_chunk;
		m_InstanceCount = per_chunk * chunk_count;
		if (m_InstanceCount == 0)
		{
			m_InstanceBuffer = nullptr;
			m_InstanceStorage = nullptr;
			m_GpuVertexView = nullptr;
			m_PadBuffer = nullptr;
			return;
		}

		for (uint32_t cz = 0; cz < m_ChunksZ; ++cz)
		{
			for (uint32_t cx = 0; cx < m_ChunksX; ++cx)
			{
				const uint32_t chunk_index = cz * m_ChunksX + cx;
				Chunk chunk;

				const glm::vec3 chunk_xz = m_Origin
					+ m_Right * (-m_HalfX + (static_cast<float>(cx) + 0.5f) * m_ChunkWidth)
					+ m_Forward * (-m_HalfZ + (static_cast<float>(cz) + 0.5f) * m_ChunkDepth);

				/* 贴合地形时包围盒按这一块下面的地形高度范围算：只按叶高算，
				 * 山坡上的草会被整块剔掉；按整张地形的范围算又太松，剔除不起作用 */
				float base_y = m_Origin.y;
				float top_y = m_Origin.y + m_Height * m_ScaleY;
				if (m_Ground != nullptr)
				{
					/* 块可能被旋转过，用包含它的轴对齐方形去查（块不大，误差可忽略） */
					const float half_span = (m_ChunkWidth + m_ChunkDepth) * 0.5f;
					const glm::vec2 xz_min(chunk_xz.x - half_span, chunk_xz.z - half_span);
					const glm::vec2 xz_max(chunk_xz.x + half_span, chunk_xz.z + half_span);

					float ground_lo = 0.0f;
					float ground_hi = 0.0f;
					if (m_Ground->GetHeightRange(xz_min, xz_max, ground_lo, ground_hi))
					{
						base_y = ground_lo;
						top_y = ground_hi + m_Height * m_ScaleY;
					}
					else
					{
						base_y = m_Ground->GetMinWorldHeight();
						top_y = m_Ground->GetMaxWorldHeight() + m_Height * m_ScaleY;
					}
				}

				chunk.Center = chunk_xz + glm::vec3(0.0f, (base_y + top_y) * 0.5f - m_Origin.y, 0.0f);
				chunk.Extent = glm::vec3(
					m_ChunkWidth * 0.5f + m_Width * m_ScaleX * 0.5f,
					(top_y - base_y) * 0.5f,
					m_ChunkDepth * 0.5f + m_Width * m_ScaleX * 0.5f);
				chunk.First = chunk_index * per_chunk;
				chunk.Count = per_chunk;
				m_Chunks.push_back(chunk);
			}
		}

		/* 块底面：每块一张四边形，铺在叶根下面；颜色由块序号散出来，
		 * 相邻块深浅略有差别，看起来不像一整块塑料 */
		/* 贴着地形时不需要块底面：草底下就是地形本身，再铺一层草色方块会跟起伏打架 */
		std::vector<GrassInstanceData> pads;
		if (m_Ground == nullptr)
			pads.reserve(m_Chunks.size());
		for (size_t chunk_index = 0; m_Ground == nullptr && chunk_index < m_Chunks.size(); ++chunk_index)
		{
			const Chunk& chunk = m_Chunks[chunk_index];
			GrassInstanceData pad;
			pad.Basis = glm::mat4(
				glm::vec4(chunk.Extent.x, 0.0f, 0.0f, 0.0f),
				glm::vec4(0.0f, 1.0f, 0.0f, 0.0f),
				glm::vec4(0.0f, 0.0f, chunk.Extent.z, 0.0f),
				glm::vec4(chunk.Center.x, m_Origin.y + 0.02f, chunk.Center.z, 1.0f));
			const uint32_t pad_seed = MixHash(static_cast<uint32_t>(chunk_index) ^ MixHash(m_Seed));
			pad.Params = glm::vec4(-1.0f, 1.0f, HashUnit(pad_seed, 1u), HashUnit(pad_seed, 2u));
			pads.push_back(pad);
		}
		m_PadBuffer = pads.empty() ? nullptr
			: DeviceVertexBuffer::Create("GrassFieldPads", pads.data(),
				static_cast<uint32_t>(pads.size() * sizeof(GrassInstanceData)));

		/* 脚印图：覆盖整片草地的世界包围盒（旋转过的草地也按包围盒算，分辨率略降） */
		if (m_TrampleTexture == nullptr)
		{
			TextureDesc desc;
			desc.Width = kTrampleSize;
			desc.Height = kTrampleSize;
			desc.Format = TextureFormat::RGBA8;
			desc.Usage = TextureUsage::Sampleable;
			m_TrampleTexture = DeviceTexture::Create("GrassTrample", desc);
			m_Trample.assign(static_cast<size_t>(kTrampleSize) * kTrampleSize * 4u, 0u);
		}

		glm::vec3 bounds_min(std::numeric_limits<float>::max());
		glm::vec3 bounds_max(-std::numeric_limits<float>::max());
		for (const float sign_x : { -1.0f, 1.0f })
		{
			for (const float sign_z : { -1.0f, 1.0f })
			{
				const glm::vec3 corner = m_Origin + m_Right * (m_HalfX * sign_x)
					+ m_Forward * (m_HalfZ * sign_z);
				bounds_min = glm::min(bounds_min, corner);
				bounds_max = glm::max(bounds_max, corner);
			}
		}
		const glm::vec2 size_2d(bounds_max.x - bounds_min.x, bounds_max.z - bounds_min.z);
		m_TrampleRect = glm::vec4(bounds_min.x, bounds_min.z,
			size_2d.x > 1e-4f ? 1.0f / size_2d.x : 0.0f,
			size_2d.y > 1e-4f ? 1.0f / size_2d.y : 0.0f);

		/* 实例数据统一放存储缓冲，顶点缓冲只是它的一块视图：
		 * GPU 放置直接往里写，间接剔除也读它，省一次来回拷 */
		const uint32_t bytes = m_InstanceCount * static_cast<uint32_t>(sizeof(GrassInstanceData));
		if (m_InstanceStorage == nullptr || bytes > m_InstanceStorage->GetDataSize())
		{
			m_InstanceStorage = DeviceStorageBuffer::Create("GrassInstances", bytes);
			m_GpuVertexView = m_InstanceStorage != nullptr
				? DeviceVertexBuffer::CreateShared("GrassInstancesView", m_InstanceStorage, bytes)
				: nullptr;
		}

		const bool shared_ok = m_InstanceStorage != nullptr && m_GpuVertexView != nullptr;
		/* GPU 放置暂时不贴地（计算里还读不到高度图），有地形时先走 CPU 路径 */
		const bool gpu_path = m_Ground == nullptr && g_GpuPlacementEnabled && shared_ok
			&& Renderer::GetRenderAPI() != nullptr && Renderer::GetRenderAPI()->SupportsCompute();
		m_UsingGpuPlacement = gpu_path;
		if (gpu_path)
		{
			m_InstanceBuffer = m_GpuVertexView;
			m_CapacityBytes = bytes;
			m_GpuDirty = true;
		}
		else if (shared_ok)
		{
			BuildCpuInstances(m_InstanceStorage);
			m_InstanceBuffer = m_GpuVertexView;
			m_CapacityBytes = bytes;
		}
		else
		{
			/* 后端给不了共享视图（比如 macOS 的 GL 4.1）：退回普通顶点缓冲 */
			m_InstanceStorage = nullptr;
			m_GpuVertexView = nullptr;
			BuildCpuInstances(nullptr);
		}

		UploadTrample();
	}

	void GrassField::BuildCpuInstances(const SharedPtr<DeviceStorageBuffer>& storage)
	{
		std::vector<GrassInstanceData> instances(m_InstanceCount);
		for (uint32_t cz = 0; cz < m_ChunksZ; ++cz)
		{
			for (uint32_t cx = 0; cx < m_ChunksX; ++cx)
			{
				const uint32_t first = (cz * m_ChunksX + cx) * m_PerChunk;
				for (uint32_t i = 0; i < m_PerChunk; ++i)
				{
					/* 每株一个基值：块坐标与序号一起混进去，同样的输入每次给同样的结果。
					 * 计算着色器里是同一套公式，两边必须一字不差 */
					const uint32_t base = MixHash(i ^ MixHash(cx ^ MixHash(cz ^ MixHash(m_Seed))));

					const float local_x = -m_HalfX
						+ (static_cast<float>(cx) + HashUnit(base, 1u)) * m_ChunkWidth;
					const float local_z = -m_HalfZ
						+ (static_cast<float>(cz) + HashUnit(base, 2u)) * m_ChunkDepth;

					const float yaw = HashUnit(base, 3u) * kTwoPi;
					const float height = m_Height * m_ScaleY * (0.7f + 0.55f * HashUnit(base, 4u));
					const float width = m_Width * m_ScaleX * (0.75f + 0.5f * HashUnit(base, 5u));

					glm::vec3 position = m_Origin + m_Right * local_x + m_Forward * local_z;

					/* 贴地：叶根落到地形表面；点不在地形范围内就留在原平面 */
					glm::vec3 ground_normal(0.0f, 1.0f, 0.0f);
					float growth = 1.0f;
					if (m_Ground != nullptr)
					{
						const glm::vec2 world_xz(position.x, position.z);
						float ground_y = position.y;
						if (m_Ground->SampleHeightWorld(world_xz, ground_y))
						{
							position.y = ground_y;

							/* 坡度过滤：太陡的地方把草退化掉。不跳过是故意的 ——
							 * 每块株数固定，GPU 侧才能用序号反推块号 */
							glm::vec3 normal(0.0f, 1.0f, 0.0f);
							if (m_Ground->SampleNormalWorld(world_xz, normal))
							{
								ground_normal = normal;
								if (m_MaxSlope > 0.0f && m_MaxSlope < 90.0f
									&& ground_normal.y < std::cos(glm::radians(m_MaxSlope)))
								{
									growth = 0.0f;
								}
							}
						}
					}

					/* 叶面姿态：按坡面法线整体旋转，SurfaceAlign 控制贴合程度（0 = 始终竖直） */
					glm::quat align = glm::identity<glm::quat>();
					if (m_Ground != nullptr && m_SurfaceAlign > 0.0f)
					{
						const glm::quat target = glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), ground_normal);
						align = glm::slerp(glm::identity<glm::quat>(), target, m_SurfaceAlign);
					}

					/* 叶片朝向决定倒向哪边；宽高已经折进基向量，着色器直接加就行 */
					const glm::vec3 dir_right = align * (m_Right * std::cos(yaw) + m_Forward * std::sin(yaw));
					const glm::vec3 dir_face = align * (-m_Right * std::sin(yaw) + m_Forward * std::cos(yaw));
					const glm::vec3 up_axis = align * glm::vec3(0.0f, 1.0f, 0.0f);

					GrassInstanceData& instance = instances[first + i];
					instance.Basis = glm::mat4(
						glm::vec4(dir_right * (width * growth), 0.0f),
						glm::vec4(up_axis * (height * growth), 0.0f),
						glm::vec4(dir_face * (width * growth), 0.0f),
						glm::vec4(position, 1.0f));
					instance.Params = glm::vec4(-1.0f, m_ReceiveShadow ? 1.0f : 0.0f,
						HashUnit(base, 6u), HashUnit(base, 7u));
				}
			}
		}

		/* 叶根高度范围、坡度退化数：贴地检查与调试用 */
		m_SlopeCulledCount = 0;
		m_InstanceMinY = std::numeric_limits<float>::max();
		m_InstanceMaxY = -std::numeric_limits<float>::max();
		for (const GrassInstanceData& instance : instances)
		{
			const float y = instance.Basis[3].y;
			m_InstanceMinY = std::min(m_InstanceMinY, y);
			m_InstanceMaxY = std::max(m_InstanceMaxY, y);
			/* 退化株的三个基向量长度都是 0 */
			if (glm::dot(instance.Basis[1], instance.Basis[1]) < 1e-8f)
				++m_SlopeCulledCount;
		}
		if (instances.empty())
		{
			m_InstanceMinY = 0.0f;
			m_InstanceMaxY = 0.0f;
		}

		const uint32_t bytes = static_cast<uint32_t>(instances.size() * sizeof(GrassInstanceData));
		if (storage != nullptr)
		{
			storage->SetData(instances.data(), bytes);
		}
		else
		{
			m_InstanceBuffer = DeviceVertexBuffer::Create("GrassFieldInstances", instances.data(), bytes);
		}
		m_CapacityBytes = bytes;
	}

	void GrassField::EnsurePlaced()
	{
		if (!m_GpuDirty || m_InstanceStorage == nullptr || m_InstanceCount == 0)
			return;

		/* 参数块与 GrassPlacement.glsl 的 std140 布局逐字节对应 */
		struct alignas(16) PlacementParams
		{
			glm::vec4 BasisRight{ 0.0f };    /* xyz 轴，w = 缩放 */
			glm::vec4 BasisUp{ 0.0f };
			glm::vec4 BasisForward{ 0.0f };
			glm::vec4 Origin{ 0.0f };
			glm::vec4 Grid{ 0.0f };          /* x 块数 x、y 块数 z、z 每块株数、w 种子 */
			glm::vec4 Shape{ 0.0f };         /* x 半宽、y 半长、z 块宽、w 块深 */
			glm::vec4 Blade{ 0.0f };         /* x 叶高、y 叶宽、z 接受阴影、w 总株数 */
		};

		PlacementParams params;
		params.BasisRight = glm::vec4(m_Right, m_ScaleX);
		params.BasisUp = glm::vec4(0.0f, 1.0f, 0.0f, m_ScaleY);
		params.BasisForward = glm::vec4(m_Forward, 1.0f);
		params.Origin = glm::vec4(m_Origin, 0.0f);
		params.Grid = glm::vec4(static_cast<float>(m_ChunksX), static_cast<float>(m_ChunksZ),
			static_cast<float>(m_PerChunk), static_cast<float>(m_Seed));
		params.Shape = glm::vec4(m_HalfX, m_HalfZ, m_ChunkWidth, m_ChunkDepth);
		params.Blade = glm::vec4(m_Height, m_Width, m_ReceiveShadow ? 1.0f : 0.0f,
			static_cast<float>(m_InstanceCount));

		if (Renderer::DispatchGrassPlacement(m_InstanceStorage, &params, sizeof(params), m_InstanceCount))
			m_GpuDirty = false;
	}

	void GrassField::PrepareIndirect(const glm::vec4* planes, const glm::vec3& eye)
	{
		m_IndirectPrepared = false;
		if (!g_IndirectDrawEnabled || planes == nullptr || m_InstanceCount == 0 || m_InstanceStorage == nullptr)
			return;
		if (Renderer::GetRenderAPI() == nullptr || !Renderer::GetRenderAPI()->SupportsCompute())
			return;

		const uint32_t bytes = m_InstanceCount * static_cast<uint32_t>(sizeof(GrassInstanceData));
		if (m_IndirectDrawBuffer == nullptr || m_IndirectDrawBuffer->GetDataSize() < bytes)
		{
			m_IndirectDrawBuffer = DeviceStorageBuffer::Create("GrassIndirectDraw", bytes);
			m_IndirectDrawView = m_IndirectDrawBuffer != nullptr
				? DeviceVertexBuffer::CreateShared("GrassIndirectView", m_IndirectDrawBuffer, bytes)
				: nullptr;
			m_IndirectArgs = DeviceStorageBuffer::Create("GrassIndirectArgs", 5u * sizeof(uint32_t));
			if (m_IndirectDrawView == nullptr || m_IndirectArgs == nullptr)
			{
				m_IndirectDrawBuffer = nullptr;
				m_IndirectArgs = nullptr;
				return;
			}
		}

		/* 参数块与 GrassIndirectCull.glsl 的 std140 布局逐字节对应 */
		struct alignas(16) CullParams
		{
			glm::vec4 Planes[6]{};
			glm::vec4 Origin{ 0.0f };
			glm::vec4 Right{ 0.0f };
			glm::vec4 Forward{ 0.0f };
			glm::vec4 Grid{ 0.0f };
			glm::vec4 Counts{ 0.0f };
			glm::vec4 Extra{ 0.0f };
			glm::vec4 Camera{ 0.0f };
		};

		CullParams params;
		for (int index = 0; index < 6; ++index)
			params.Planes[index] = planes[index];
		params.Origin = glm::vec4(m_Origin, 0.0f);
		params.Right = glm::vec4(m_Right, m_HalfX);
		params.Forward = glm::vec4(m_Forward, m_HalfZ);
		params.Grid = glm::vec4(m_ChunkWidth, m_ChunkDepth,
			static_cast<float>(m_PerChunk), static_cast<float>(m_InstanceCount));
		params.Counts = glm::vec4(static_cast<float>(m_ChunksX), static_cast<float>(m_ChunksZ),
			m_LodDistance, m_CullDistance);
		params.Extra = glm::vec4(m_MinDensityScale, m_Height * m_ScaleY, 0.0f, 0.0f);
		params.Camera = glm::vec4(eye, 0.0f);

		if (!Renderer::DispatchGrassArgsReset(m_IndirectArgs, kBladeIndexCount))
			return;
		m_IndirectPrepared = Renderer::DispatchGrassCull(m_InstanceStorage, m_IndirectDrawBuffer,
			m_IndirectArgs, &params, sizeof(params), m_InstanceCount);
	}

	void GrassField::StampBend(const glm::vec3& world_position, float radius)
	{
		if (m_Trample.empty() || m_TrampleTexture == nullptr || radius <= 0.0f)
			return;
		if (m_TrampleRect.z <= 0.0f || m_TrampleRect.w <= 0.0f)
			return;

		/* 世界尺寸与格子尺寸：格子坐标 = (世界 - 最小角) * 1/尺寸 * 分辨率 */
		const glm::vec2 rect_min(m_TrampleRect.x, m_TrampleRect.y);
		const glm::vec2 world_size(1.0f / m_TrampleRect.z, 1.0f / m_TrampleRect.w);
		const glm::vec2 cell_size(world_size.x / kTrampleSize, world_size.y / kTrampleSize);
		const glm::vec2 rel = glm::vec2(world_position.x, world_position.z) - rect_min;
		if (rel.x < -radius || rel.y < -radius
			|| rel.x > world_size.x + radius || rel.y > world_size.y + radius)
			return;

		const int32_t center_x = static_cast<int32_t>(rel.x / cell_size.x);
		const int32_t center_y = static_cast<int32_t>(rel.y / cell_size.y);
		const int32_t cell_radius = static_cast<int32_t>(radius / std::min(cell_size.x, cell_size.y)) + 1;
		const int32_t lo_x = std::max(0, center_x - cell_radius);
		const int32_t hi_x = std::min<int32_t>(kTrampleSize - 1, center_x + cell_radius);
		const int32_t lo_y = std::max(0, center_y - cell_radius);
		const int32_t hi_y = std::min<int32_t>(kTrampleSize - 1, center_y + cell_radius);

		for (int32_t y = lo_y; y <= hi_y; ++y)
		{
			for (int32_t x = lo_x; x <= hi_x; ++x)
			{
				const glm::vec2 cell_center = rect_min
					+ glm::vec2((static_cast<float>(x) + 0.5f) * cell_size.x,
						(static_cast<float>(y) + 0.5f) * cell_size.y);
				const glm::vec2 delta = cell_center - glm::vec2(world_position.x, world_position.z);
				const float distance = glm::length(delta);
				if (distance >= radius)
					continue;

				/* 前半段压满、后半段线性收边：平方衰减掉得太快，
				 * 物体自身半径外就几乎没力度，看起来像什么都没压 */
				const float weight = std::min(1.0f, (1.0f - distance / radius) * 2.0f);
				const glm::vec2 direction = distance > 1e-4f ? delta / distance : glm::vec2(0.0f);

				uint8_t* pixel = &m_Trample[(static_cast<size_t>(y) * kTrampleSize + x) * 4u];
				pixel[0] = static_cast<uint8_t>((direction.x * 0.5f + 0.5f) * 255.0f + 0.5f);
				pixel[1] = static_cast<uint8_t>((direction.y * 0.5f + 0.5f) * 255.0f + 0.5f);
				pixel[2] = std::max<uint8_t>(pixel[2], static_cast<uint8_t>(weight * 255.0f + 0.5f));
			}
		}

		/* 当场传上去：踩下去这一帧就该看见，不等下一帧的衰减顺带上传 */
		UploadTrample();
	}

	void GrassField::TickBend(float delta_time, float recover_seconds)
	{
		if (m_Trample.empty() || m_TrampleTexture == nullptr)
			return;

		/* 指数衰减：站着不动时每帧被重新踩深，走开之后按 recover_seconds 慢慢弹回 */
		const float recover = std::max(recover_seconds, 0.01f);
		const float factor = std::exp(-delta_time / recover);

		bool any_left = false;
		for (size_t i = 2; i < m_Trample.size(); i += 4)
		{
			if (m_Trample[i] == 0)
				continue;

			/* 衰减到 8 位精度以下就直接归零：不然永远差一口气，弹不回原状 */
			const auto decayed = static_cast<float>(m_Trample[i]) * factor;
			m_Trample[i] = decayed < 2.0f ? 0u : static_cast<uint8_t>(decayed);
			any_left = any_left || m_Trample[i] != 0;
		}

		/* 全弹回去了就整张清掉，之后不再每帧上传 */
		if (!any_left)
			std::fill(m_Trample.begin(), m_Trample.end(), 0u);

		UploadTrample();
	}

	void GrassField::UploadTrample()
	{
		if (m_TrampleTexture == nullptr || m_Trample.empty())
			return;
		m_TrampleTexture->SetData(m_Trample.data(), { PixelFormat::RGBA, PixelType::UnsignedByte }, 0);
	}

	const MeshPrimitive& GrassField::GetPadPrimitive()
	{
		/* 单位四边形铺在 XZ 平面上、范围 [-1, 1]：实例矩阵把它拉伸成每块草地的底面 */
		static const MeshPrimitive s_Primitive = []()
		{
			static constexpr float vertices[] = {
				-1.0f, 0.0f, -1.0f,
				-1.0f, 0.0f,  1.0f,
				 1.0f, 0.0f,  1.0f,
				 1.0f, 0.0f, -1.0f,
			};
			static constexpr uint16_t indices[] = { 0, 1, 2, 0, 2, 3 };

			const auto vertex_buffer = DeviceVertexBuffer::Create("GrassPad_VertexBuffer",
				vertices, static_cast<uint32_t>(sizeof(vertices)));
			vertex_buffer->SetLayout(VertexBufferLayout{
				{ "a_Position", BufferDataType::Float3 },
			});

			const auto index_buffer = IndexBuffer::Create("GrassPad_IndexBuffer", indices,
				static_cast<uint32_t>(std::size(indices)), IndexType::UInt16);

			auto vertex_array = DeviceVertexArray::Create("GrassPad_VertexArray");
			vertex_array->AddVertexBuffer(vertex_buffer);
			vertex_array->SetIndexBuffer(index_buffer);
			return MeshPrimitive(vertex_array);
		}();
		return s_Primitive;
	}

	const MeshPrimitive& GrassField::GetBladePrimitive()
	{
		/* 叶片模板：x ∈ [-1, 1] 是叶宽方向，y ∈ [0, 1] 从根到梢。
		 * 实际形状由顶点着色器按实例数据展开，这里只要一根 4 段的窄条。 */
		static const MeshPrimitive s_Primitive = []()
		{
			constexpr int kSegments = 4;
			std::vector<float> vertices;
			vertices.reserve((kSegments + 1) * 2 * 3);
			for (int i = 0; i <= kSegments; ++i)
			{
				const float y = static_cast<float>(i) / static_cast<float>(kSegments);
				vertices.insert(vertices.end(), { -1.0f, y, 0.0f, 1.0f, y, 0.0f });
			}

			std::vector<uint16_t> indices;
			indices.reserve(kSegments * 6);
			for (int i = 0; i < kSegments; ++i)
			{
				const uint16_t a = static_cast<uint16_t>(i * 2);
				const uint16_t b = static_cast<uint16_t>(i * 2 + 1);
				const uint16_t c = static_cast<uint16_t>(i * 2 + 2);
				const uint16_t d = static_cast<uint16_t>(i * 2 + 3);
				indices.insert(indices.end(), { a, b, c, b, d, c });
			}

			const auto vertex_buffer = DeviceVertexBuffer::Create("GrassBlade_VertexBuffer",
				vertices.data(), static_cast<uint32_t>(vertices.size() * sizeof(float)));
			vertex_buffer->SetLayout(VertexBufferLayout{
				{ "a_Position", BufferDataType::Float3 },
			});

			const auto index_buffer = IndexBuffer::Create("GrassBlade_IndexBuffer", indices.data(),
				static_cast<uint32_t>(indices.size()), IndexType::UInt16);

			auto vertex_array = DeviceVertexArray::Create("GrassBlade_VertexArray");
			vertex_array->AddVertexBuffer(vertex_buffer);
			vertex_array->SetIndexBuffer(index_buffer);
			return MeshPrimitive(vertex_array);
		}();
		return s_Primitive;
	}
}
