#include "Pch.h"
#include "SkeletonModel.h"
#include "Scene.h"
#include "Entity.h"
#include "Components.h"
#include "Mesh.h"
#include "Model.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/Renderer/RenderView.h"
#include "Helios/Renderer/FrameGraph/FrameGraph.h"
#include "Helios/VirtualDevice/DeviceBuffer.h"
#include "Helios/VirtualDevice/DeviceVertexArray.h"
#include <fstream>
#include <unordered_map>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>

namespace Helios
{
	namespace
	{

		bool WriteString(std::ostream& stream, const std::string& text)
		{
			const uint32_t length = static_cast<uint32_t>(text.size());
			stream.write(reinterpret_cast<const char*>(&length), sizeof(length));
			stream.write(text.data(), static_cast<std::streamsize>(length));
			return static_cast<bool>(stream);
		}

		bool ReadString(std::istream& stream, std::string& out_text)
		{
			uint32_t length = 0;
			stream.read(reinterpret_cast<char*>(&length), sizeof(length));
			if (!stream || length > (1u << 20))
				return false;

			out_text.resize(length);
			stream.read(out_text.data(), static_cast<std::streamsize>(length));
			return static_cast<bool>(stream);
		}

		/* 调色板是否和上一帧一致（矩阵逐元素比，误差容忍很小） */
		bool PaletteEqual(const std::vector<glm::mat4>& lhs, const std::vector<glm::mat4>& rhs)
		{
			if (lhs.size() != rhs.size())
				return false;

			for (size_t index = 0; index < lhs.size(); ++index)
			{
				for (int column = 0; column < 4; ++column)
				{
					const glm::vec4& a = lhs[index][column];
					const glm::vec4& b = rhs[index][column];
					if (std::fabs(a.x - b.x) > 1.0e-6f || std::fabs(a.y - b.y) > 1.0e-6f
						|| std::fabs(a.z - b.z) > 1.0e-6f || std::fabs(a.w - b.w) > 1.0e-6f)
						return false;
				}
			}
			return true;
		}
	}

	/* ==================== SkeletonData：资产层的骨骼数据 ==================== */

	int SkeletonData::FindJoint(const std::string& name) const
	{
		for (size_t index = 0; index < m_Joints.size(); ++index)
		{
			if (m_Joints[index].Name == name)
				return static_cast<int>(index);
		}
		return -1;
	}

	std::vector<glm::mat4> SkeletonData::BuildPalette(const std::vector<glm::mat4>& joint_world) const
	{
		std::vector<glm::mat4> palette(m_Joints.size(), glm::mat4(1.0f));
		for (size_t index = 0; index < m_Joints.size(); ++index)
		{
			const glm::mat4 world = (index < joint_world.size()) ? joint_world[index] : glm::mat4(1.0f);
			palette[index] = m_MeshBindMatrix * world * m_Joints[index].InverseBind;
		}
		return palette;
	}

	glm::vec3 SkeletonData::SkinPosition(const glm::vec3& position, const SkinWeight& weight,
		const std::vector<glm::mat4>& palette)
	{
		glm::vec3 skinned(0.0f);
		float total = 0.0f;
		for (int influence = 0; influence < 4; ++influence)
		{
			const float w = weight.Weights[influence];
			if (w <= 0.0f)
				continue;

			const size_t joint = weight.Joints[influence];
			if (joint >= palette.size())
				continue;

			skinned += w * glm::vec3(palette[joint] * glm::vec4(position, 1.0f));
			total += w;
		}

		if (total <= 1.0e-6f)
			return position;

		return skinned / total;
	}

	bool SkeletonData::Write(std::ostream& stream) const
	{
		stream.write(kSkeletonFileMagic, sizeof(kSkeletonFileMagic));
		stream.write(reinterpret_cast<const char*>(&m_MeshBindMatrix), sizeof(glm::mat4));

		const uint32_t joint_count = static_cast<uint32_t>(m_Joints.size());
		stream.write(reinterpret_cast<const char*>(&joint_count), sizeof(joint_count));
		for (const SkinJoint& joint : m_Joints)
		{
			WriteString(stream, joint.Name);
			stream.write(reinterpret_cast<const char*>(&joint.InverseBind), sizeof(glm::mat4));
			stream.write(reinterpret_cast<const char*>(&joint.Parent), sizeof(joint.Parent));
			stream.write(reinterpret_cast<const char*>(&joint.RestLocal), sizeof(glm::mat4));
		}

		const uint32_t segment_count = static_cast<uint32_t>(m_Segments.size());
		stream.write(reinterpret_cast<const char*>(&segment_count), sizeof(segment_count));
		for (const SkinSegment& segment : m_Segments)
		{
			WriteString(stream, segment.Name);
			const uint32_t vertex_count = static_cast<uint32_t>(segment.Vertices.size());
			stream.write(reinterpret_cast<const char*>(&vertex_count), sizeof(vertex_count));
			if (vertex_count > 0)
			{
				stream.write(reinterpret_cast<const char*>(segment.Vertices.data()),
					static_cast<std::streamsize>(sizeof(SkinWeight) * vertex_count));
			}
		}

		return static_cast<bool>(stream);
	}

	bool SkeletonData::Read(std::istream& stream)
	{
		char magic[4]{};
		stream.read(magic, sizeof(magic));
		if (!stream || std::memcmp(magic, kSkeletonFileMagic, sizeof(magic)) != 0)
			return false;

		stream.read(reinterpret_cast<char*>(&m_MeshBindMatrix), sizeof(glm::mat4));
		if (!stream)
			return false;

		uint32_t joint_count = 0;
		stream.read(reinterpret_cast<char*>(&joint_count), sizeof(joint_count));
		if (!stream || joint_count > (1u << 20))
			return false;

		m_Joints.clear();
		m_Joints.resize(joint_count);
		for (SkinJoint& joint : m_Joints)
		{
			if (!ReadString(stream, joint.Name))
				return false;
			stream.read(reinterpret_cast<char*>(&joint.InverseBind), sizeof(glm::mat4));
			if (!stream)
				return false;

			stream.read(reinterpret_cast<char*>(&joint.Parent), sizeof(joint.Parent));
			if (!stream)
				return false;

			stream.read(reinterpret_cast<char*>(&joint.RestLocal), sizeof(glm::mat4));
			if (!stream)
				return false;
		}

		uint32_t segment_count = 0;
		stream.read(reinterpret_cast<char*>(&segment_count), sizeof(segment_count));
		if (!stream || segment_count > (1u << 16))
			return false;

		m_Segments.clear();
		m_Segments.resize(segment_count);
		for (SkinSegment& segment : m_Segments)
		{
			if (!ReadString(stream, segment.Name))
				return false;

			uint32_t vertex_count = 0;
			stream.read(reinterpret_cast<char*>(&vertex_count), sizeof(vertex_count));
			if (!stream)
				return false;

			segment.Vertices.resize(vertex_count);
			if (vertex_count > 0)
			{
				stream.read(reinterpret_cast<char*>(segment.Vertices.data()),
					static_cast<std::streamsize>(sizeof(SkinWeight) * vertex_count));
			}
			if (!stream)
				return false;
		}

		return true;
	}

	/* ==================== SkeletonModel：实例数据 + 骨骼操作 ==================== */

	namespace
	{
		/* 全局蒙皮开关：默认走 GPU，编辑器菜单可切到 CPU */
		bool g_GlobalGpuSkinningEnabled = true;
	}

	void SkeletonModel::SetGlobalGpuSkinningEnabled(bool enabled)
	{
		g_GlobalGpuSkinningEnabled = enabled;
	}

	bool SkeletonModel::IsGlobalGpuSkinningEnabled()
	{
		return g_GlobalGpuSkinningEnabled;
	}

	SkeletonModel::SkeletonModel(std::string path)
		: Model(std::move(path))
	{
	}

	SharedPtr<SkeletonModel> SkeletonModel::Create(const std::string& path)
	{
		/* 共用 Model::Create 的装载流程：带骨骼段的 .mesh 会造出 SkeletonModel。
		 * 每次调用都是一个新实例，运行时数据（动态网格 / 关节）不会在实体间串。 */
		SharedPtr<Model> model = Model::Create(path);
		if (model == nullptr || model->IsStaticModel() || !model->HasSkin())
			return nullptr;

		return std::static_pointer_cast<SkeletonModel>(model);
	}

	void SkeletonModel::EnsureSkinnedSegments()
	{
		if (!HasSkin())
			return;

		const auto& segments = GetMeshSegments();
		if (m_SkinnedSegments.size() == segments.size()
			&& m_SkinnedVertices.size() == m_SkinnedSegments.size())
			return;

		m_SkinnedSegments.clear();
		m_SkinnedVertices.clear();
		m_GpuSegments.clear();
		m_GpuReady = false;
		m_SkinnedSegments.reserve(segments.size());
		m_SkinnedVertices.reserve(segments.size());

		const SkeletonData& data = GetSkin();
		const auto& skin_segments = data.GetSegments();

		/* 调色板：最多 64 根（着色器数组长度） */
		bool gpu_possible = data.GetJoints().size() <= kMaxGpuJoints;
		SharedPtr<DeviceStorageBuffer> palette_buffer;
		if (gpu_possible)
		{
			palette_buffer = DeviceStorageBuffer::Create("SkinPalette", kMaxGpuJoints * sizeof(glm::mat4));
			gpu_possible = palette_buffer != nullptr;
		}

		for (size_t segment_index = 0; segment_index < segments.size(); ++segment_index)
		{
			const SharedPtr<MeshSegment>& source = segments[segment_index];
			const MeshPrimitive& primitive = source->GetMeshPrimitive();
			const auto& source_buffers = primitive.VertexArray->GetVertexBuffers();
			if (source_buffers.empty())
				continue;

			const uint32_t vertex_count = static_cast<uint32_t>(source->GetSourceVertices().size() / 16);
			const uint32_t vertex_bytes = vertex_count * 16 * sizeof(float);

			/* 先准备 GPU 那套：上屏缓冲和顶点缓冲共用同一块显存，省一次拷贝 */
			SharedPtr<DeviceStorageBuffer> bind_buffer;
			SharedPtr<DeviceStorageBuffer> weight_buffer;
			SharedPtr<DeviceStorageBuffer> skinned_storage;
			SharedPtr<DeviceVertexBuffer> skinned_buffer;

			if (gpu_possible && vertex_count > 0 && segment_index < skin_segments.size())
			{
				bind_buffer = DeviceStorageBuffer::Create(source->GetDebugName() + "_BindVerts",
					source->GetSourceVertices().data(), vertex_bytes);
				skinned_storage = DeviceStorageBuffer::Create(source->GetDebugName() + "_SkinnedVerts", vertex_bytes);

				/* 权重：每顶点 4 关节下标 + 4 权重的 float 数组 */
				std::vector<float> packed(vertex_count * 8, 0.0f);
				const SkinSegment& skin_segment = skin_segments[segment_index];
				for (uint32_t vertex_index = 0; vertex_index < vertex_count
					&& vertex_index < skin_segment.Vertices.size(); ++vertex_index)
				{
					const SkinWeight& weight = skin_segment.Vertices[vertex_index];
					float* target = packed.data() + vertex_index * 8;
					for (int influence = 0; influence < 4; ++influence)
					{
						target[influence] = static_cast<float>(weight.Joints[influence]);
						target[4 + influence] = weight.Weights[influence];
					}
				}
				weight_buffer = DeviceStorageBuffer::Create(source->GetDebugName() + "_SkinWeights",
					packed.data(), static_cast<uint32_t>(packed.size() * sizeof(float)));

				if (bind_buffer != nullptr && weight_buffer != nullptr && skinned_storage != nullptr)
					skinned_buffer = DeviceVertexBuffer::CreateShared(source->GetDebugName() + "_Skinned",
						skinned_storage, vertex_bytes);
			}

			if (skinned_buffer != nullptr)
			{
				/* GPU 缓冲初始是空的：如果计算派发没生效，读回来就是 0（检查据此判断） */
				skinned_buffer->SetLayout(source_buffers.front()->GetLayout());
				m_GpuSegments.push_back(GpuSegment{ bind_buffer, weight_buffer, skinned_storage, vertex_count });
			}
			else
			{
				/* CPU 兜底：普通可写顶点缓冲 */
				skinned_buffer = DeviceVertexBuffer::Create(source->GetDebugName() + "_Skinned",
					source_buffers.front()->GetDataSize());
				skinned_buffer->SetLayout(source_buffers.front()->GetLayout());
				skinned_buffer->SetData(source->GetSourceVertices().data(),
					static_cast<uint32_t>(source->GetSourceVertices().size() * sizeof(float)));
				m_GpuSegments.push_back(GpuSegment{});
			}

			SharedPtr<DeviceVertexArray> skinned_array = DeviceVertexArray::Create(
				source->GetDebugName() + "_SkinnedVA");
			skinned_array->AddVertexBuffer(skinned_buffer);
			skinned_array->SetIndexBuffer(primitive.VertexArray->GetIndexBuffer());

			m_SkinnedSegments.push_back(MeshSegment::Create(
				source->GetDebugName() + "_Skinned", MeshPrimitive(skinned_array, primitive.PrimitiveType),
				source->GetSlotIndex()));
			m_SkinnedVertices.push_back(source->GetSourceVertices());
		}

		/* 所有段都建好 GPU 缓冲才算就绪 */
		m_GpuPalette = palette_buffer;
		m_GpuReady = palette_buffer != nullptr && !m_GpuSegments.empty();
		for (const GpuSegment& segment : m_GpuSegments)
		{
			if (segment.Skinned == nullptr)
			{
				m_GpuReady = false;
				break;
			}
		}
	}

	void* SkeletonModel::GetGpuSkinnedData(size_t segment) const
	{
		if (segment >= m_GpuSegments.size() || m_GpuSegments[segment].Skinned == nullptr)
			return nullptr;
		return m_GpuSegments[segment].Skinned->GetMappedData();
	}

	std::vector<entt::entity> SkeletonModel::CreateJointEntities(Scene& scene, entt::entity root,
		const SkeletonData& data)
	{
		const std::vector<SkinJoint>& table = data.GetJoints();
		std::vector<entt::entity> joints;
		if (table.empty() || !scene.IsEntityValid(root))
			return joints;

		joints.reserve(table.size());

		/* 静置姿态的世界矩阵：从根的静置局部逐级累乘（根相对模型实体） */
		std::vector<glm::mat4> rest_world(table.size(), glm::mat4(1.0f));
		for (size_t index = 0; index < table.size(); ++index)
		{
			const int parent = table[index].Parent;
			const glm::mat4 parent_world = (parent >= 0 && parent < static_cast<int>(index))
				? rest_world[parent] : glm::mat4(1.0f);
			rest_world[index] = parent_world * table[index].RestLocal;
		}

		/* 1. 建实体（变换先留单位阵） */
		for (size_t index = 0; index < table.size(); ++index)
		{
			const std::string name = table[index].Name.empty()
				? ("Joint_" + std::to_string(index)) : table[index].Name;
			Entity entity = scene.CreateEntity(name);
			joints.push_back(static_cast<entt::entity>(entity));
		}

		/* 2. 挂父子：Scene::SetParent 会保持世界变换、重算局部变换，
		 *    所以局部变换放到挂完之后再写 */
		for (size_t index = 0; index < table.size(); ++index)
		{
			const int parent = table[index].Parent;
			const entt::entity parent_entity = (parent >= 0 && parent < static_cast<int>(joints.size()))
				? joints[parent] : root;

			Entity child{ joints[index], scene.shared_from_this() };
			Entity parent_handle{ parent_entity, scene.shared_from_this() };
			scene.SetParent(child, parent_handle);
		}

		/* 3. 局部变换直接用静置局部（动画写的也是它所在的空间） */
		for (size_t index = 0; index < table.size(); ++index)
		{
			const glm::mat4 local = table[index].RestLocal;

			glm::vec3 scale(1.0f);
			glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
			glm::vec3 position(0.0f);
			glm::vec3 skew(0.0f);
			glm::vec4 perspective(0.0f);
			glm::decompose(local, scale, rotation, position, skew, perspective);

			Entity entity{ joints[index], scene.shared_from_this() };
			TransformComponent& transform = entity.GetComponent<TransformComponent>();
			transform.m_Position = position;
			transform.m_Rotation = glm::eulerAngles(rotation);
			transform.m_Scale = scale;
		}

		return joints;
	}

	std::vector<glm::mat4> SkeletonModel::CollectJointTransforms(const Scene& scene,
		const std::vector<entt::entity>& joints, entt::entity root)
	{
		const glm::mat4 root_inverse = scene.IsEntityValid(root)
			? glm::inverse(scene.GetWorldTransform(root)) : glm::mat4(1.0f);

		std::vector<glm::mat4> matrices(joints.size(), glm::mat4(1.0f));
		for (size_t index = 0; index < joints.size(); ++index)
		{
			if (scene.IsEntityValid(joints[index]))
				matrices[index] = root_inverse * scene.GetWorldTransform(joints[index]);
		}
		return matrices;
	}

	std::vector<entt::entity> SkeletonModel::ResolveJoints(Scene& scene, entt::entity owner) const
	{
		const SkeletonData& data = GetSkin();

		/* 关节是嵌套的（根 → 椎 → 四肢），要从模型实体往下整棵子树找 */
		std::unordered_map<std::string, entt::entity> children;
		auto parent_view = scene.GetRegistry().view<ParentComponent>();

		std::vector<entt::entity> pending{ owner };
		while (!pending.empty())
		{
			const entt::entity current = pending.back();
			pending.pop_back();

			for (auto child : parent_view)
			{
				if (parent_view.get<ParentComponent>(child).m_Parent != current)
					continue;

				if (const auto* name = scene.GetRegistry().try_get<NameComponent>(child))
					children[name->m_Name] = child;
				pending.push_back(child);
			}
		}

		std::vector<entt::entity> joints(data.GetJoints().size(),
			static_cast<entt::entity>(entt::null));
		for (size_t index = 0; index < data.GetJoints().size(); ++index)
		{
			const auto found = children.find(data.GetJoints()[index].Name);
			if (found != children.end())
				joints[index] = found->second;
		}
		return joints;
	}

	void SkeletonModel::Rebind(Scene& scene, entt::entity owner)
	{
		/* 先把实体下面现有的关节实体都销毁：可能是上一个模型、或重新导入前的旧绑定数据 */
		std::vector<entt::entity> stale = ResolveJoints(scene, owner);
		for (entt::entity joint : m_Joints)
			stale.push_back(joint);

		for (entt::entity joint : stale)
		{
			if (!scene.IsEntityValid(joint))
				continue;

			Entity joint_entity{ joint, scene.shared_from_this() };
			scene.DestroyEntity(joint_entity);
		}

		/* 旧的动态网格、调色板都作废，下一帧按新数据重建 */
		m_SkinnedSegments.clear();
		m_SkinnedVertices.clear();
		m_GpuSegments.clear();
		m_GpuPalette.reset();
		m_GpuReady = false;
		m_Palette.clear();

		m_Joints = CreateJointEntities(scene, owner, GetSkin());
	}

	void SkeletonModel::UpdatePose(Scene& scene, entt::entity owner)
	{
		const SkeletonData& data = GetSkin();

		/* 第一次用到这个模型（刚挂上 / 刚重新导入）：按当前关节表重建一遍骨骼树，
		 * 保证实体里的关节和这次导入的绑定数据、权重是同一份 */
		if (!m_AutoBindTried)
		{
			m_AutoBindTried = true;
			Rebind(scene, owner);
		}

		EnsureSkinnedSegments();
		if (m_SkinnedSegments.empty())
			return;

		m_Joints = ResolveJoints(scene, owner);

		std::vector<glm::mat4> joint_world = CollectJointTransforms(scene, m_Joints, owner);

		/* 没绑上的关节按绑定姿态补：调色板是单位阵，顶点保持原位（不至于被逆绑定矩阵拉成一团） */
		for (size_t index = 0; index < m_Joints.size() && index < joint_world.size(); ++index)
		{
			if (!scene.IsEntityValid(m_Joints[index]))
				joint_world[index] = glm::inverse(data.GetJoints()[index].InverseBind);
		}

		std::vector<glm::mat4> palette = data.BuildPalette(joint_world);

		/* 姿势没变就不重算顶点、也不重传缓冲；刚切换过 GPU/CPU 则强制重算一次 */
		const bool gpu_active = m_GpuSkinningEnabled && IsGlobalGpuSkinningEnabled();
		if (PaletteEqual(palette, m_Palette) && gpu_active == m_LastGpuActive)
			return;
		m_LastGpuActive = gpu_active;
		m_Palette = std::move(palette);

		/* GPU 蒙皮：调色板先传上去，顶点留给渲染图的蒙皮 Pass 算（绘制前一定跑过）；
		 * 派发不成立时那一趟会退回 CPU，所以这里不用等结果 */
		if (gpu_active && m_GpuReady && m_GpuPalette != nullptr
			&& m_Palette.size() <= kMaxGpuJoints && m_GpuSegments.size() == m_SkinnedSegments.size())
		{
			std::vector<glm::mat4> padded(kMaxGpuJoints, glm::mat4(1.0f));
			std::copy(m_Palette.begin(), m_Palette.end(), padded.begin());
			m_GpuPalette->SetData(padded.data(), static_cast<uint32_t>(padded.size() * sizeof(glm::mat4)));
			m_GpuSkinPending = true;
			return;
		}

		SkinOnCpu();
	}

	/* 派发这一帧的 GPU 蒙皮：渲染图的蒙皮 Pass 每帧跑一次，没有待算的直接跳过 */
	bool SkeletonModel::DispatchGpuSkin()
	{
		if (!m_GpuSkinPending)
			return false;

		m_GpuSkinPending = false;

		/* 缓冲在两帧之间重建过（换网格 / 换模型）：这一帧先按 CPU 算 */
		if (!m_GpuReady || m_GpuPalette == nullptr || m_GpuSegments.size() != m_SkinnedSegments.size())
		{
			SkinOnCpu();
			return false;
		}

		for (const GpuSegment& segment : m_GpuSegments)
		{
			if (!Renderer::DispatchSkin(m_GpuPalette, segment.BindVertices, segment.Weights,
				segment.Skinned, segment.VertexCount))
			{
				/* 后端不支持 / 着色器没编过：这一帧退回 CPU，顶点不至于停在旧姿势 */
				SkinOnCpu();
				return false;
			}
		}

		return true;
	}

	/* CPU 蒙皮：按当前调色板把每个顶点算回模型空间，再上传到动态顶点缓冲 */
	void SkeletonModel::SkinOnCpu()
	{
		const auto& skin_segments = GetSkin().GetSegments();
		const auto& source_segments = GetMeshSegments();
		for (size_t segment_index = 0; segment_index < m_SkinnedSegments.size(); ++segment_index)
		{
			if (segment_index >= skin_segments.size() || segment_index >= source_segments.size())
				break;

			const SkinSegment& skin_segment = skin_segments[segment_index];
			std::vector<float>& vertices = m_SkinnedVertices[segment_index];
			const std::vector<float>& bind_vertices = source_segments[segment_index]->GetSourceVertices();
			if (vertices.size() != bind_vertices.size() || vertices.empty())
				continue;

			const size_t vertex_count = vertices.size() / 16;
			for (size_t vertex_index = 0; vertex_index < vertex_count; ++vertex_index)
			{
				const float* source = bind_vertices.data() + vertex_index * 16;
				float* target = vertices.data() + vertex_index * 16;

				/* 位置 / 法线 / 切线跟着调色板走，颜色与 UV 原样搬过去 */
				glm::vec3 position(0.0f);
				glm::vec3 normal(0.0f);
				glm::vec3 tangent(0.0f);
				float total = 0.0f;

				const SkinWeight& weight = skin_segment.Vertices[vertex_index];
				for (int influence = 0; influence < 4; ++influence)
				{
					const float w = weight.Weights[influence];
					if (w <= 0.0f)
						continue;

					const size_t joint = weight.Joints[influence];
					if (joint >= m_Palette.size())
						continue;

					const glm::mat4& matrix = m_Palette[joint];
					position += w * glm::vec3(matrix * glm::vec4(source[0], source[1], source[2], 1.0f));
					normal += w * glm::vec3(matrix * glm::vec4(source[3], source[4], source[5], 0.0f));
					tangent += w * glm::vec3(matrix * glm::vec4(source[13], source[14], source[15], 0.0f));
					total += w;
				}

				if (total > 1.0e-6f)
				{
					position /= total;
					normal = glm::normalize(normal);
					tangent = glm::normalize(tangent);
				}
				else
				{
					position = glm::vec3(source[0], source[1], source[2]);
					normal = glm::vec3(source[3], source[4], source[5]);
					tangent = glm::vec3(source[13], source[14], source[15]);
				}

				target[0] = position.x;
				target[1] = position.y;
				target[2] = position.z;
				target[3] = normal.x;
				target[4] = normal.y;
				target[5] = normal.z;
				target[6] = source[6];
				target[7] = source[7];
				target[8] = source[8];
				target[9] = source[9];
				target[10] = source[10];
				target[11] = source[11];
				target[12] = source[12];
				target[13] = tangent.x;
				target[14] = tangent.y;
				target[15] = tangent.z;
			}

			const auto& buffers = m_SkinnedSegments[segment_index]->GetMeshPrimitive()
				.VertexArray->GetVertexBuffers();
			if (!buffers.empty())
			{
				buffers.front()->SetData(vertices.data(),
					static_cast<uint32_t>(vertices.size() * sizeof(float)));
			}
		}
	}

	void SkeletonModel::UpdateScene(Scene& scene)
	{
		auto& registry = scene.GetRegistry();
		auto view = registry.view<ModelComponent>();

		for (auto entity : view)
		{
			ModelComponent& component = view.get<ModelComponent>(entity);
			if (component.m_Model == nullptr || !component.m_EnableSkinning || component.m_Model->IsStaticModel())
				continue;

			static_cast<SkeletonModel*>(component.m_Model.get())->UpdatePose(scene, entity);
		}
	}

	namespace
	{
		/* 场景里有没有走 GPU 蒙皮的模型：决定渲染图里要不要挂蒙皮这一趟 */
		bool HasGpuSkinnedModel(Scene& scene)
		{
			auto view = scene.GetRegistry().view<ModelComponent>();
			for (auto entity : view)
			{
				ModelComponent& component = view.get<ModelComponent>(entity);
				if (component.m_Model == nullptr || !component.m_EnableSkinning || component.m_Model->IsStaticModel())
					continue;

				const auto* skeleton = static_cast<const SkeletonModel*>(component.m_Model.get());
				if (skeleton->IsGpuSkinned() && skeleton->IsGpuSkinningEnabled())
					return true;
			}
			return false;
		}
	}

	void SkeletonModel::AddGpuSkinPass(RenderView& render_view)
	{
		if (!IsGlobalGpuSkinningEnabled() || Renderer::CurrentAPI() != RenderAPI::Metal)
			return;

		const SharedPtr<Scene> scene = render_view.GetOwnerScene();
		if (scene == nullptr || !HasGpuSkinnedModel(*scene))
			return;

		struct GpuSkinPassData {};

		render_view.GetFrameGraph()->AddPass<GpuSkinPassData>("GPUSkinPass",
			[](FrameGraphBuilder& builder, GpuSkinPassData&)
			{
				/* 结果写在蒙皮缓冲里，图里没有资源可绑：显式保活，按计算 Pass 收尾 */
				builder.AsSideEffect();
				builder.AsComputePass();
			},
			[scene](const FrameGraphResources&, const GpuSkinPassData&)
			{
				auto& registry = scene->GetRegistry();
				auto view = registry.view<ModelComponent>();
				for (auto entity : view)
				{
					ModelComponent& component = view.get<ModelComponent>(entity);
					if (component.m_Model == nullptr || !component.m_EnableSkinning || component.m_Model->IsStaticModel())
						continue;

					static_cast<SkeletonModel*>(component.m_Model.get())->DispatchGpuSkin();
				}
			});
	}

	void SkeletonModel::Clear(Scene& scene, entt::entity entity)
	{
		auto* component = scene.GetRegistry().try_get<ModelComponent>(entity);
		if (component == nullptr || component->m_Model == nullptr || component->m_Model->IsStaticModel())
			return;

		SkeletonModel* skeleton = static_cast<SkeletonModel*>(component->m_Model.get());

		/* 关节实体是建在模型实体下面的：换模型 / 删实体时一并销毁，别在场景里留孤儿。
		 * 嵌套关系无所谓：DestroyEntity 会把子节点提成根节点，按列表逐个删干净即可。 */
		const std::vector<entt::entity> joints = skeleton->GetJointEntities();
		for (entt::entity joint : joints)
		{
			if (!scene.IsEntityValid(joint))
				continue;

			Entity joint_entity{ joint, scene.shared_from_this() };
			scene.DestroyEntity(joint_entity);
		}

		skeleton->GetJointEntities().clear();
		skeleton->GetSkinnedVertices().clear();
		skeleton->GetPalette().clear();
	}
}
