#include "Pch.h"
#include "AstcBaker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include "Helios/Common/PathUtils.h"
#include "Helios/Core/AstcFile.h"
#include "Helios/Core/Jobs.h"
#include "Helios/Core/TextureSettings.h"

#include <astcenc.h>
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize.h>

namespace Helios::AstcBake
{
	namespace
	{
		/* 一次只跑一个烘焙：界面每帧从 s_Status 取进度 */
		std::mutex s_Mutex;
		Status s_Status;
		/* 后台任务句柄：只在主线程的 Start 里写，任务本身靠状态跟界面通信 */
		Jobs::Task s_Task;

		bool IsFile(const std::filesystem::path& path)
		{
			std::error_code error;
			return std::filesystem::is_regular_file(path, error);
		}

		int MipCount(int width, int height)
		{
			int count = 1;
			int longest = std::max(width, height);
			while (longest > 1)
			{
				longest /= 2;
				++count;
			}
			return count;
		}

		void AppendU24(std::string& out, uint32_t value)
		{
			for (int index = 0; index < 3; ++index)
				out.push_back(static_cast<char>((value >> (8 * index)) & 0xff));
		}

		void AppendU32(std::string& out, uint32_t value)
		{
			for (int index = 0; index < 4; ++index)
				out.push_back(static_cast<char>((value >> (8 * index)) & 0xff));
		}

		bool ReadFile(const std::filesystem::path& path, std::vector<unsigned char>& out)
		{
			std::ifstream file(path, std::ios::binary | std::ios::ate);
			if (!file)
				return false;

			const std::streamsize size = file.tellg();
			if (size <= 0)
				return false;

			out.resize(static_cast<size_t>(size));
			file.seekg(0);
			return static_cast<bool>(file.read(reinterpret_cast<char*>(out.data()), size));
		}

		/* 缩一级：宽高各减半，走 stb 的默认滤波（边缘钳位也交给它） */
		void Downsample(const std::vector<unsigned char>& source, int source_width, int source_height,
			std::vector<unsigned char>& target, int target_width, int target_height)
		{
			target.resize(static_cast<size_t>(target_width) * target_height * 4);
			stbir_resize_uint8(source.data(), source_width, source_height, 0,
				target.data(), target_width, target_height, 0, 4);
		}

		/* 一次烘焙共用一个上下文：配置只跟块尺寸有关，各级图像换着喂进去，
		 * 每级编完 reset 一次，用不着重建上下文 */
		class Encoder
		{
		public:
			Encoder(int block_x, int block_y, std::string& error)
			{
				astcenc_error result = astcenc_config_init(ASTCENC_PRF_LDR,
					static_cast<unsigned int>(block_x), static_cast<unsigned int>(block_y), 1,
					ASTCENC_PRE_MEDIUM, 0, &m_Config);
				if (result != ASTCENC_SUCCESS)
				{
					error = astcenc_get_error_string(result);
					return;
				}

				/* 留两核给编辑器：烘焙排在后台，不该把机器吃干 */
				const unsigned int cores = std::max(1u, std::thread::hardware_concurrency());
				m_Threads = std::clamp(cores > 2 ? cores - 2 : 1u, 1u, 8u);
				result = astcenc_context_alloc(&m_Config, m_Threads, &m_Context, nullptr);
				if (result != ASTCENC_SUCCESS)
				{
					m_Context = nullptr;
					error = astcenc_get_error_string(result);
				}
			}

			~Encoder()
			{
				if (m_Context != nullptr)
					astcenc_context_free(m_Context);
			}

			Encoder(const Encoder&) = delete;
			Encoder& operator=(const Encoder&) = delete;

			[[nodiscard]] bool IsReady() const { return m_Context != nullptr; }

			/* 编一张图：块数据直接进内存，不再落临时文件 */
			bool Compress(int width, int height, const std::vector<unsigned char>& pixels,
				std::vector<unsigned char>& out, std::string& error)
			{
				astcenc_image image{};
				image.dim_x = static_cast<unsigned int>(width);
				image.dim_y = static_cast<unsigned int>(height);
				image.dim_z = 1;
				image.data_type = ASTCENC_TYPE_U8;
				void* slices[1] = { const_cast<unsigned char*>(pixels.data()) };
				image.data = slices;

				const astcenc_swizzle swizzle{ ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };
				const int block_x = static_cast<int>(m_Config.block_x);
				const int block_y = static_cast<int>(m_Config.block_y);
				const size_t blocks = static_cast<size_t>((width + block_x - 1) / block_x)
					* static_cast<size_t>((height + block_y - 1) / block_y);
				out.assign(blocks * 16, 0);

				/* 多线程按线程号各调一次，astcenc 自己把块分给这几条线程 */
				std::vector<astcenc_error> codes(m_Threads, ASTCENC_SUCCESS);
				std::vector<std::thread> workers;
				workers.reserve(m_Threads);
				for (unsigned int index = 0; index < m_Threads; ++index)
				{
					workers.emplace_back([this, &image, &swizzle, &out, &codes, index]
					{
						codes[index] = astcenc_compress_image(m_Context, &image, &swizzle,
							out.data(), out.size(), index);
					});
				}
				for (std::thread& worker : workers)
					worker.join();

				for (const astcenc_error code : codes)
				{
					if (code != ASTCENC_SUCCESS)
					{
						error = astcenc_get_error_string(code);
						return false;
					}
				}

				astcenc_compress_reset(m_Context);
				return true;
			}

		private:
			astcenc_config m_Config{};
			astcenc_context* m_Context{ nullptr };
			unsigned int m_Threads{ 1 };
		};

		void SetFailed(const std::string& message)
		{
			std::lock_guard lock(s_Mutex);
			s_Status.Current = Status::State::Failed;
			s_Status.Message = message;
		}

		/* 被取消（过期 / 退出）：状态回到空闲，别让面板停在 Running */
		void SetIdle()
		{
			std::lock_guard lock(s_Mutex);
			s_Status = Status{};
		}

		/* 任务没跑成（取消 / 池满被丢）时没人会改状态：读写状态前顺手收尾。
		 * 只在主线程持锁调用 */
		void ReapSkippedLocked()
		{
			if (s_Status.Current != Status::State::Running)
				return;
			if (!s_Task.IsDone() || s_Task.State() == Jobs::TaskState::Completed)
				return;
			s_Status = Status{};
		}

		/* 后台线程主体：解码 -> 缩放 -> 逐级编码 -> 拼容器 -> 落盘 */
		void Bake(const std::string& source_path, const Options& options)
		{
			std::filesystem::path source;
			if (!TryPathFromUtf8(source_path, source) || !IsFile(source))
			{
				SetFailed("source file missing");
				return;
			}

			std::vector<unsigned char> file_bytes;
			if (!ReadFile(source, file_bytes))
			{
				SetFailed("cannot read source file");
				return;
			}

			/* 行序跟加载器同一口径：PNG 路径按 Flip V 翻转后上传，块数据直接传，所以烘的时候就得翻 */
			stbi_set_flip_vertically_on_load_thread(options.FlipVertical ? 1 : 0);

			int source_width = 0;
			int source_height = 0;
			int channels = 0;
			stbi_uc* decoded = stbi_load_from_memory(file_bytes.data(), static_cast<int>(file_bytes.size()),
				&source_width, &source_height, &channels, 4);
			if (decoded == nullptr || source_width <= 0 || source_height <= 0)
			{
				stbi_image_free(decoded);
				SetFailed("cannot decode image");
				return;
			}

			/* 目标尺寸：长边不超过 MaxSize，比例不变 */
			int width = source_width;
			int height = source_height;
			if (options.MaxSize > 0 && std::max(width, height) > options.MaxSize)
			{
				const double scale = static_cast<double>(options.MaxSize) / std::max(width, height);
				width = std::max(1, static_cast<int>(std::lround(source_width * scale)));
				height = std::max(1, static_cast<int>(std::lround(source_height * scale)));
			}

			std::vector<unsigned char> level_pixels(decoded, decoded + static_cast<size_t>(source_width) * source_height * 4);
			stbi_image_free(decoded);

			if (width != source_width || height != source_height)
			{
				std::vector<unsigned char> resized;
				Downsample(level_pixels, source_width, source_height, resized, width, height);
				level_pixels.swap(resized);
			}

			const int level_count = MipCount(width, height);
			{
				std::lock_guard lock(s_Mutex);
				s_Status.Width = width;
				s_Status.Height = height;
				s_Status.LevelCount = level_count;
			}

			std::string error;
			Encoder encoder(options.BlockX, options.BlockY, error);
			if (!encoder.IsReady())
			{
				SetFailed(error.empty() ? "cannot create encoder" : error);
				return;
			}

			/* 头 16 字节：magic + 块尺寸 + 尺寸 + 深度（2D 恒为 1） */
			std::string container;
			AppendU32(container, AstcFile::kMagic);
			container.push_back(static_cast<char>(options.BlockX));
			container.push_back(static_cast<char>(options.BlockY));
			container.push_back(1);
			AppendU24(container, static_cast<uint32_t>(width));
			AppendU24(container, static_cast<uint32_t>(height));
			AppendU24(container, 1);

			int level_width = width;
			int level_height = height;
			for (int level = 0; level < level_count; ++level)
			{
				/* 一级几秒，取消只在这条缝里响应 */
				if (Jobs::IsCancelled())
				{
					SetIdle();
					return;
				}

				std::vector<unsigned char> encoded;
				if (!encoder.Compress(level_width, level_height, level_pixels, encoded, error))
				{
					SetFailed(error.empty() ? "compression failed" : error);
					return;
				}
				container.append(reinterpret_cast<const char*>(encoded.data()), encoded.size());

				{
					std::lock_guard lock(s_Mutex);
					s_Status.Level = level + 1;
				}

				if (level + 1 >= level_count)
					break;

				const int next_width = std::max(1, level_width / 2);
				const int next_height = std::max(1, level_height / 2);
				std::vector<unsigned char> next_pixels;
				Downsample(level_pixels, level_width, level_height, next_pixels, next_width, next_height);
				level_pixels.swap(next_pixels);
				level_width = next_width;
				level_height = next_height;
			}

			if (Jobs::IsCancelled())
			{
				SetIdle();
				return;
			}

			/* 先写临时文件再改名：引擎随时可能来读，不能让它看到半个文件 */
			std::filesystem::path target = source;
			target.replace_extension(".astc");
			const std::filesystem::path staging = target.string() + ".tmp";

			{
				std::ofstream file(staging, std::ios::binary | std::ios::trunc);
				if (!file || !file.write(container.data(), static_cast<std::streamsize>(container.size())))
				{
					SetFailed("cannot write " + PathToUtf8(target));
					return;
				}
			}

			std::error_code error_code;
			std::filesystem::remove(target, error_code);
			std::filesystem::rename(staging, target, error_code);
			if (error_code)
			{
				SetFailed("cannot replace " + PathToUtf8(target));
				return;
			}

			{
				std::lock_guard lock(s_Mutex);
				s_Status.Current = Status::State::Succeeded;
				s_Status.OutputPath = PathToUtf8(target);
				s_Status.Bytes = static_cast<long long>(container.size());
			}
		}
	}

	bool SourceSize(const std::string& source_path, int& width, int& height)
	{
		std::filesystem::path source;
		std::vector<unsigned char> bytes;
		if (!TryPathFromUtf8(source_path, source) || !ReadFile(source, bytes))
			return false;

		int channels = 0;
		return stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels) != 0;
	}

	BakedInfo InspectAstc(const std::string& astc_path)
	{
		BakedInfo info;

		std::filesystem::path path;
		std::error_code error;
		if (!TryPathFromUtf8(astc_path, path) || !std::filesystem::is_regular_file(path, error))
			return info;

		/* 只读 16 字节头：面板每帧都问一次，别整份文件读进来 */
		AstcFile::Header header;
		if (!AstcFile::ReadHeader(path, header))
			return info;

		info.Exists = true;
		info.BlockX = header.BlockX;
		info.BlockY = header.BlockY;
		info.Width = header.Width;
		info.Height = header.Height;
		info.Levels = MipCount(header.Width, header.Height);
		info.Bytes = static_cast<long long>(std::filesystem::file_size(path, error));
		return info;
	}

	BakedInfo InspectBaked(const std::string& source_path)
	{
		std::filesystem::path source;
		if (!TryPathFromUtf8(source_path, source))
			return BakedInfo{};

		std::filesystem::path baked = source;
		baked.replace_extension(".astc");

		BakedInfo info = InspectAstc(PathToUtf8(baked));
		if (!info.Exists)
			return info;

		std::error_code baked_error;
		std::error_code source_error;
		const auto baked_time = std::filesystem::last_write_time(baked, baked_error);
		const auto source_time = std::filesystem::last_write_time(source, source_error);
		bool stale = !baked_error && !source_error && baked_time < source_time;

		/* 伴生设置比产物新也算过期：改了 Flip V 这类数据布局就得重烘 */
		std::filesystem::path meta_path;
		std::error_code meta_error;
		if (TryPathFromUtf8(TextureSettingsStore::MetaPath(source_path), meta_path)
			&& std::filesystem::is_regular_file(meta_path, meta_error))
		{
			const auto meta_time = std::filesystem::last_write_time(meta_path, meta_error);
			stale = stale || (!meta_error && !baked_error && meta_time > baked_time);
		}

		info.Stale = stale;
		return info;
	}

	long long EstimateBytes(int width, int height, int block_x, int block_y)
	{
		if (width <= 0 || height <= 0 || block_x <= 0 || block_y <= 0)
			return 0;

		long long bytes = static_cast<long long>(AstcFile::kHeaderBytes);
		for (int level = 0; level < MipCount(width, height); ++level)
		{
			const int level_width = std::max(1, width >> level);
			const int level_height = std::max(1, height >> level);
			bytes += static_cast<long long>((level_width + block_x - 1) / block_x)
				* ((level_height + block_y - 1) / block_y) * 16;
		}
		return bytes;
	}

	bool Start(const std::string& source_path, const Options& options)
	{
		{
			std::lock_guard lock(s_Mutex);
			ReapSkippedLocked();
			if (s_Status.Current == Status::State::Running)
				return false;

			s_Status = Status{};
			s_Status.SourcePath = source_path;
			s_Status.Current = Status::State::Running;
		}

		/* 走引擎的后台任务通道：低优先级，切工程时被 InvalidateBackground 收掉；
		 * 没建池的场合（检查程序）就地执行，语义不变 */
		s_Task = Jobs::RunBackground([source_path, options] { Bake(source_path, options); },
			Jobs::Priority::Low);
		return true;
	}

	Status Poll()
	{
		std::lock_guard lock(s_Mutex);
		ReapSkippedLocked();
		return s_Status;
	}
}
