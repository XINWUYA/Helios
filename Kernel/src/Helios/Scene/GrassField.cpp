#include "Pch.h"
#include "GrassField.h"
#include "TerrainField.h"
#include "TerrainSampling.h"
#include "Helios/Core/Jobs.h"
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
		const TerrainField* ground, const glm::vec2& focus_min, const glm::vec2& focus_max)
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
		mix(&component.m_FollowTerrain, sizeof(component.m_FollowTerrain));
		mix(&component.m_MaxInstances, sizeof(component.m_MaxInstances));
		/* 接受阴影写进实例参数，改了要重建才生效 */
		mix(&component.m_ReceiveShadow, sizeof(component.m_ReceiveShadow));
		mix(&g_GpuPlacementEnabled, sizeof(g_GpuPlacementEnabled));
		mix(glm::value_ptr(world), sizeof(glm::mat4));
		/* 地形换了一份或重建过：贴地高度要重新采样 */
		const uint64_t ground_key = ground != nullptr ? ground->GetRevision() : 0ull;
		mix(&ground_key, sizeof(ground_key));
		/* 观察范围按 32 米量化：相机小范围移动不触发重建，跨格了才重铺 */
		const glm::vec2 focus_min_key(std::floor(focus_min.x / 32.0f), std::floor(focus_min.y / 32.0f));
		const glm::vec2 focus_max_key(std::floor(focus_max.x / 32.0f), std::floor(focus_max.y / 32.0f));
		mix(&focus_min_key, sizeof(focus_min_key));
		mix(&focus_max_key, sizeof(focus_max_key));

		/* 后台重建的结果先收：key 还对得上才用，对不上说明参数又变了，直接丢 */
		if (m_AsyncBuild != nullptr && m_AsyncBuild->Done.load(std::memory_order_acquire))
		{
			std::vector<GrassInstanceData> instances = std::move(m_AsyncBuild->Instances);
			m_AsyncBuild.reset();
			if (m_AsyncKey == m_BuildKey && m_InstanceStorage != nullptr)
			{
				UploadInstances(instances, m_InstanceStorage);
				m_InstanceBuffer = m_GpuVertexView;
				m_CapacityBytes = static_cast<uint32_t>(instances.size() * sizeof(GrassInstanceData));
			}
		}

		if (key == m_BuildKey && m_InstanceBuffer != nullptr)
			return;
		m_BuildKey = key;
		m_Ground = ground;
		m_FocusMin = focus_min;
		m_FocusMax = focus_max;

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

		/* 跟随地形：范围取地形覆盖范围。
		 * 块尺寸按剔除距离推：块必须明显小于 CullDistance，否则剔除按块中心判定时，
		 * 相机身边只有一两块能通过，视野剩下全是空的。地形 patch 的粒度不参与这里。 */
		m_ChunkSize = kChunkSize;
		if (component.m_FollowTerrain && m_Ground != nullptr)
		{
			m_Origin.x = m_Ground->GetOrigin().x;
			m_Origin.z = m_Ground->GetOrigin().z;
			m_HalfX = m_Ground->GetExtent().x;
			m_HalfZ = m_Ground->GetExtent().y;
			m_ChunkSize = std::clamp(component.m_CullDistance * 0.25f, kChunkSize, 64.0f);
		}
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

		m_ChunksX = std::max(1u, static_cast<uint32_t>(std::ceil(m_HalfX * 2.0f / m_ChunkSize)));
		m_ChunksZ = std::max(1u, static_cast<uint32_t>(std::ceil(m_HalfZ * 2.0f / m_ChunkSize)));
		m_ChunkWidth = m_HalfX * 2.0f / static_cast<float>(m_ChunksX);
		m_ChunkDepth = m_HalfZ * 2.0f / static_cast<float>(m_ChunksZ);

		const uint32_t total_chunks = m_ChunksX * m_ChunksZ;
		const float chunk_radius = 0.5f * glm::length(glm::vec2(m_ChunkWidth, m_ChunkDepth));
		const float cull_radius = std::max(component.m_CullDistance, m_ChunkWidth) + chunk_radius;

		/* 有效块：观察点周围 CullDistance 内的块。全地形均匀铺满在 1km² 上放不下预算，
		 * 远处本来也会被剔除距离砍掉，干脆不生成。
		 * 只在跟随地形时裁剪：矩形草地范围本来就小，没必要跟着相机反复重建 */
		const bool crop_to_focus = component.m_FollowTerrain && m_Ground != nullptr;
		std::vector<uint8_t> active(total_chunks, 0);
		uint32_t active_count = 0;
		const auto chunk_center_xz = [this](uint32_t cx, uint32_t cz)
		{
			return m_Origin
				+ m_Right * (-m_HalfX + (static_cast<float>(cx) + 0.5f) * m_ChunkWidth)
				+ m_Forward * (-m_HalfZ + (static_cast<float>(cz) + 0.5f) * m_ChunkDepth);
		};
		for (uint32_t cz = 0; cz < m_ChunksZ; ++cz)
		{
			for (uint32_t cx = 0; cx < m_ChunksX; ++cx)
			{
				const glm::vec3 center = chunk_center_xz(cx, cz);
				const glm::vec2 center_xz(center.x, center.z);
				/* 到观察范围（多相机并集的矩形）的距离：范围内算 0 */
				const glm::vec2 closest = glm::clamp(center_xz, m_FocusMin, m_FocusMax);
				const bool inside = !crop_to_focus
					|| glm::length(center_xz - closest) <= cull_radius;
				if (inside)
				{
					active[cz * m_ChunksX + cx] = 1;
					++active_count;
				}
			}
		}
		m_CroppedToFocus = active_count < total_chunks;

		/* 每块株数：先按密度算，再按「有效块数」分摊预算 —— 只被视野内的块分，
		 * 近处的密度才不会因为整片地形太大而被摊薄 */
		uint32_t per_chunk = static_cast<uint32_t>(
			std::max(0.0f, std::round(density * m_ChunkWidth * m_ChunkDepth)));
		const uint32_t budget = std::min(kMaxInstances, std::max(1024u, component.m_MaxInstances));
		const uint32_t share = std::max(1u, active_count);
		while (per_chunk > 0 && per_chunk * share > budget)
			--per_chunk;
		m_PerChunk = per_chunk;

		/* 只给有效块分配实例区间：块表仍按整片地形建（剔除要用），远处的 Count = 0 */
		uint32_t next_first = 0;
		for (uint32_t cz = 0; cz < m_ChunksZ; ++cz)
		{
			for (uint32_t cx = 0; cx < m_ChunksX; ++cx)
			{
				const uint32_t chunk_index = cz * m_ChunksX + cx;
				Chunk chunk;

				const glm::vec3 chunk_xz = chunk_center_xz(cx, cz);

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

				if (active[chunk_index] != 0 && per_chunk > 0)
				{
					chunk.First = next_first;
					chunk.Count = per_chunk;
					next_first += per_chunk;
				}
				else
				{
					chunk.First = 0;
					chunk.Count = 0;
				}
				m_Chunks.push_back(chunk);
			}
		}

		m_InstanceCount = next_first;
		if (m_InstanceCount == 0)
		{
			m_InstanceBuffer = nullptr;
			m_InstanceStorage = nullptr;
			m_GpuVertexView = nullptr;
			m_PadBuffer = nullptr;
			return;
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
		/* 贴地时把地形高度上传成 storage buffer；建不出来就退回 CPU 路径 */
		const bool ground_ready = m_Ground == nullptr || m_Ground->GetHeightStorage() != nullptr;
		/* 按观察点裁剪后块号不再连续，GPU 放置的「序号 ÷ 每块株数」会推错块，改走 CPU */
		const bool gpu_path = ground_ready && !m_CroppedToFocus && g_GpuPlacementEnabled && shared_ok
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
			/* 株数多的时候把生成丢后台：拖地形笔刷会反复触发重建，同步算会卡主线程。
			 * 算完之前不画草（旧缓冲长度对不上新株数，留着会越界） */
			constexpr uint32_t kAsyncThreshold = 20000;
			if (m_InstanceCount >= kAsyncThreshold && Jobs::IsInitialized() && m_AsyncBuild == nullptr)
			{
				auto state = CreateSharedPtr<AsyncBuild>();
				state->Key = m_BuildKey;
				m_AsyncKey = m_BuildKey;
				m_AsyncBuild = state;

				const BuildParams params = CaptureBuildParams();
				Jobs::RunBackground([state, params]()
				{
					std::vector<GrassInstanceData> instances = GenerateInstances(params);
					if (Jobs::IsCancelled())
						return;
					state->Instances = std::move(instances);
					state->Done.store(true, std::memory_order_release);
				});

				m_InstanceBuffer = nullptr;
				m_CapacityBytes = 0;
				UploadTrample();
				return;
			}

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

	GrassField::BuildParams GrassField::CaptureBuildParams() const
	{
		BuildParams params;
		params.Origin = m_Origin;
		params.Right = m_Right;
		params.Forward = m_Forward;
		params.HalfX = m_HalfX;
		params.HalfZ = m_HalfZ;
		params.ChunkWidth = m_ChunkWidth;
		params.ChunkDepth = m_ChunkDepth;
		params.Height = m_Height;
		params.Width = m_Width;
		params.ScaleX = m_ScaleX;
		params.ScaleY = m_ScaleY;
		params.SurfaceAlign = m_SurfaceAlign;
		params.MaxSlope = m_MaxSlope;
		params.ChunksX = m_ChunksX;
		params.ChunksZ = m_ChunksZ;
		params.PerChunk = m_PerChunk;
		params.InstanceCount = m_InstanceCount;
		params.Seed = m_Seed;
		params.ReceiveShadow = m_ReceiveShadow;
		params.ChunkRanges.reserve(m_Chunks.size());
		for (const Chunk& chunk : m_Chunks)
			params.ChunkRanges.emplace_back(chunk.First, chunk.Count);

		if (m_Ground != nullptr)
		{
			params.HasGround = true;
			params.GroundOrigin = m_Ground->GetOrigin();
			params.GroundExtent = m_Ground->GetExtent();
			params.GroundResolution = m_Ground->GetResolution();
			params.GroundHeightScale = m_Ground->GetHeightScale();
			/* 拷一份高度数据：后台任务不能持有地形指针（地形可能被重建或销毁） */
			params.GroundHeights = m_Ground->CopyHeights();
		}
		return params;
	}

	std::vector<GrassInstanceData> GrassField::GenerateInstances(const BuildParams& params)
	{
		PROFILE_FUNCTION();

		std::vector<GrassInstanceData> instances(params.InstanceCount);
		const float* heights = params.GroundHeights.empty() ? nullptr : params.GroundHeights.data();

		for (uint32_t chunk_index = 0; chunk_index < params.ChunkRanges.size(); ++chunk_index)
		{
			const glm::uvec2 range = params.ChunkRanges[chunk_index];
			if (range.y == 0)
				continue;

			const uint32_t cx = chunk_index % params.ChunksX;
			const uint32_t cz = chunk_index / params.ChunksX;
			for (uint32_t i = 0; i < range.y; ++i)
			{
				{
					/* 每株一个基值：块坐标与序号一起混进去，同样的输入每次给同样的结果。
					 * 计算着色器里是同一套公式，两边必须一字不差 */
					const uint32_t base = MixHash(i ^ MixHash(cx ^ MixHash(cz ^ MixHash(params.Seed))));

					const float local_x = -params.HalfX
						+ (static_cast<float>(cx) + HashUnit(base, 1u)) * params.ChunkWidth;
					const float local_z = -params.HalfZ
						+ (static_cast<float>(cz) + HashUnit(base, 2u)) * params.ChunkDepth;

					const float yaw = HashUnit(base, 3u) * kTwoPi;
					const float height = params.Height * params.ScaleY * (0.7f + 0.55f * HashUnit(base, 4u));
					const float width = params.Width * params.ScaleX * (0.75f + 0.5f * HashUnit(base, 5u));

					glm::vec3 position = params.Origin + params.Right * local_x + params.Forward * local_z;

					/* 贴地：叶根落到地形表面；点不在地形范围内就留在原平面 */
					glm::vec3 ground_normal(0.0f, 1.0f, 0.0f);
					float growth = 1.0f;
					if (params.HasGround)
					{
						const glm::vec2 world_xz(position.x, position.z);
						float ground_y = position.y;
						if (TerrainSampling::SampleHeight(heights, params.GroundResolution,
							params.GroundOrigin, params.GroundExtent, params.GroundHeightScale,
							world_xz, ground_y))
						{
							position.y = ground_y;

							/* 坡度过滤：太陡的地方把草退化掉。不跳过是故意的 ——
							 * 每块株数固定，GPU 侧才能用序号反推块号 */
							glm::vec3 normal(0.0f, 1.0f, 0.0f);
							if (TerrainSampling::SampleNormal(heights, params.GroundResolution,
								params.GroundOrigin, params.GroundExtent, params.GroundHeightScale,
								world_xz, normal))
							{
								ground_normal = normal;
								if (params.MaxSlope > 0.0f && params.MaxSlope < 90.0f
									&& ground_normal.y < std::cos(glm::radians(params.MaxSlope)))
								{
									growth = 0.0f;
								}
							}
						}
					}

					/* 叶面姿态：up 向坡面法线插值，右/前向对 up 正交化。
					 * 这套公式在 GrassPlacement.glsl 里一字不差，两条路径才长得一样 */
					const glm::vec3 up_axis = glm::normalize(glm::mix(glm::vec3(0.0f, 1.0f, 0.0f),
						ground_normal, params.SurfaceAlign));
					glm::vec3 dir_right = params.Right * std::cos(yaw) + params.Forward * std::sin(yaw);
					dir_right = glm::normalize(dir_right - up_axis * glm::dot(dir_right, up_axis));
					const glm::vec3 dir_face = glm::cross(dir_right, up_axis);

					GrassInstanceData& instance = instances[range.x + i];
					instance.Basis = glm::mat4(
						glm::vec4(dir_right * (width * growth), 0.0f),
						glm::vec4(up_axis * (height * growth), 0.0f),
						glm::vec4(dir_face * (width * growth), 0.0f),
						glm::vec4(position, 1.0f));
					instance.Params = glm::vec4(-1.0f, params.ReceiveShadow ? 1.0f : 0.0f,
						HashUnit(base, 6u), HashUnit(base, 7u));
				}
			}
		}
		return instances;
	}

	void GrassField::UploadInstances(std::vector<GrassInstanceData>& instances,
		const SharedPtr<DeviceStorageBuffer>& storage)
	{
		/* 叶根高度范围、坡度退化数：贴地检查与调试用 */
		m_SlopeCulledCount = 0;
		m_InstanceMinY = std::numeric_limits<float>::max();
		m_InstanceMaxY = -std::numeric_limits<float>::max();
		for (const GrassInstanceData& instance : instances)
		{
			const float y = instance.Basis[3].y;
			m_InstanceMinY = std::min(m_InstanceMinY, y);
			m_InstanceMaxY = std::max(m_InstanceMaxY, y);
			/* 退化株的基向量长度都是 0 */
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

	void GrassField::BuildCpuInstances(const SharedPtr<DeviceStorageBuffer>& storage)
	{
		const BuildParams params = CaptureBuildParams();
		std::vector<GrassInstanceData> instances = GenerateInstances(params);
		UploadInstances(instances, storage);
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
			glm::vec4 Terrain{ 0.0f };       /* xyz 地形中心、w 起伏总高差 */
			glm::vec4 TerrainShape{ 0.0f };  /* x 半宽、y 半长、z 分辨率、w 是否有地形 */
			glm::vec4 GrassShape{ 0.0f };    /* x 贴面程度、y cos(最大坡度)、z 启用坡度过滤 */
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

		SharedPtr<DeviceStorageBuffer> terrain_heights;
		if (m_Ground != nullptr)
		{
			terrain_heights = m_Ground->GetHeightStorage();
			params.Terrain = glm::vec4(m_Ground->GetOrigin(), m_Ground->GetHeightScale());
			params.TerrainShape = glm::vec4(m_Ground->GetExtent().x, m_Ground->GetExtent().y,
				static_cast<float>(m_Ground->GetResolution()), 1.0f);
			const bool slope_filter = m_MaxSlope > 0.0f && m_MaxSlope < 90.0f;
			params.GrassShape = glm::vec4(m_SurfaceAlign,
				slope_filter ? std::cos(glm::radians(m_MaxSlope)) : -1.0f,
				slope_filter ? 1.0f : 0.0f, 0.0f);
		}

		if (Renderer::DispatchGrassPlacement(m_InstanceStorage, &params, sizeof(params), m_InstanceCount,
			terrain_heights))
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
