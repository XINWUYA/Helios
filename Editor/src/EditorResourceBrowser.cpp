#include "Pch.h"
#include "EditorResourceBrowser.h"

namespace Helios
{
	void EditorResourceBrowser::OnImGuiRenderer()
	{
		PROFILE_FUNCTION();

        /* 创建根文件目录节点 */
		if (m_IsDirty)
		{
			m_RootFileNodeTree = CreateSharedPtr<FileNode>(g_AssetsPath.string(), "", FileType::Folder, 0, -1);
			BuildFileNodeTree(m_RootFileNodeTree);
		}

		/* 文件目录列表窗口 */
		ImGui::Begin("File List");
		{
			if (ImGui::CollapsingHeader(g_AssetsPath.string().c_str()))
			{
				/* 遍历文件节点构建UI */
				BuildFileUIListTreeSimple(m_RootFileNodeTree);
			}
		}
		ImGui::End();

		/* 详细资源列表窗口 */
		ImGui::Begin("Resource Browser");
		{
			if (!m_CurrentFileNode)
				m_CurrentFileNode = m_RootFileNodeTree; /* 默认为根节点 */

			/* 上一级目录图标 */
			{
				const bool disable_return_btn = (*m_CurrentFileNode) == (*m_RootFileNodeTree);

                /* Button样式 */
                float button_alpha = 0.5f;
				if (disable_return_btn)
				{
					ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
					button_alpha = 0.25f;
				}

				START_STYLE_ALPHA(button_alpha);
				const auto return_icon = TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH("EditorRes/icons/return.png"));
				if (ImGui::ImageButton((ImTextureID)return_icon.get(), ImVec2(20, 20), ImVec2(0, 1), ImVec2(1, 0)))
					m_CurrentFileNode = m_CurrentFileNode->ParentNode.lock();
				END_STYLE_ALPHA;

				/* 取消当前禁用样式 */
				if (disable_return_btn)
                    ImGui::PopItemFlag();
			}

			/* 计算面板宽度 */
			const float panel_width = ImGui::GetContentRegionAvail().x;

			/* 图标大小图标 */
			static float thumbnail_size = 64.0f;
			static float padding = 32.0f;

			/* 透明Button样式 */
			START_TRANSPARENT_BUTTON;

			/* 筛选控件 */
			static ImGuiTextFilter filter;
			{
				ImGui::SameLine(40, 20);
				filter.Draw("##", 200);
				ImGui::SameLine(236);
				const auto filter_icon = TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH("EditorRes/icons/filter.png"));

				START_STYLE_ALPHA(0.5f);
				ImGui::Image((ImTextureID)filter_icon.get(), ImVec2(20, 20), ImVec2(0, 1), ImVec2(1, 0));
				END_STYLE_ALPHA;
			}

			/* 图标大小图标控件 */
			{
				ImGui::SameLine(panel_width - 15);
				START_STYLE_ALPHA(0.5f);
				const auto menu_icon = TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH("EditorRes/icons/menu.png"));
				if (ImGui::ImageButton((ImTextureID)menu_icon.get(), ImVec2(20, 20), ImVec2(0, 1), ImVec2(1, 0)))
					ImGui::OpenPopup("SettingPopup");
				END_STYLE_ALPHA;

				/* 展开菜单时才显示控件 */
				if (ImGui::BeginPopup("SettingPopup"))
                {
					ImGui::PushItemWidth(120);

					ImGui::SliderFloat("Size", &thumbnail_size, 16, 128);
					ImGui::Separator();
					ImGui::SliderFloat("Padding", &padding, 0, 32);

					ImGui::PopItemWidth();
					ImGui::EndPopup();
				}
			}

			/* 取消当前透明样式 */
			END_TRANSPARENT_BUTTON;
			/* 分隔线 */
			ImGui::Separator();

			/* 文件UI, 足够大时显示图标，否则采用单列表显示 */
			if (thumbnail_size > 32)
			{
				const float cell_size = thumbnail_size + padding;
				int column_count = static_cast<int>(panel_width / cell_size);
				if (column_count < 1)
					column_count = 1;

				ImGui::Columns(column_count, 0, false);

				for (const auto& child_node : m_CurrentFileNode->ChildNodes)
				{
					auto filepath = std::filesystem::path(child_node->FilePath);

					/* 通过筛选的才需要显示 */
					if (filter.PassFilter(child_node->FilePath.c_str()))
					{
						ImGui::PushID(child_node->FileName.c_str());
						{
							ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                            {
                                /* 图标 */
                                ImGui::ImageButton((ImTextureID)child_node->Icon.get(), ImVec2(thumbnail_size, thumbnail_size), ImVec2(0, 1), ImVec2(1, 0));
                                
                                /* 拖拽 */
                                if (ImGui::BeginDragDropSource())
                                {
                                    const char* item_path = child_node->FilePath.c_str();
                                    ImGui::SetDragDropPayload("RESOURCE_BROWSER_ITEM", item_path, strlen(item_path) + 1);
                                    ImGui::EndDragDropSource();
                                }
                            }
                            ImGui::PopStyleColor();

							/* 双击响应 */
							if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
							{
                                if (child_node->Type == FileType::Folder)
									m_CurrentFileNode = child_node;
							}

							/* 右键选中菜单 */
							if (ImGui::BeginPopupContextItem())
							{
								/* 在系统文件夹中显示 */
								if (ImGui::MenuItem("Show in file explorer"))
								{
									auto path = g_AssetsPath / child_node->FilePath;
									path = absolute(path).make_preferred();
									OpenFileExplorer(path.string().c_str());
								}

								ImGui::EndPopup();
							}

							/* 文件名 */
							ImGui::TextWrapped(child_node->FileName.c_str());

							/* 下一个文件 */
							ImGui::NextColumn();
						}
						ImGui::PopID();
					}
				}
				ImGui::Columns(1);
			}
			else /* 当图标大小时，转换为列表模式 */
			{
				ImGui::BeginTable("Assets List", 3);
				{
					ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_NoHide);
					ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);
					ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed);
					ImGui::TableHeadersRow();

					/* 遍历文件节点构建UI */
					BuildFileUIListTreeDetail(m_CurrentFileNode);
				}
				ImGui::EndTable();
			}
		}
		ImGui::End();
	}

	void EditorResourceBrowser::BuildFileNodeTree(const SharedPtr<FileNode>& parent_node)
	{
		for (auto& directory_entry : std::filesystem::directory_iterator(g_AssetsPath / parent_node->FilePath))
		{
			const auto& path = directory_entry.path();
			auto relative_path = GetRelativePath(g_AssetsPath, path);
			auto filename = relative_path.filename();

			/* 创建一个文件节点 */
			auto file_node = CreateSharedPtr<FileNode>();
			file_node->ParentNode = parent_node;
			file_node->FileName = filename.string();
			file_node->FilePath = relative_path.string();
			file_node->Depth = parent_node->Depth + 1;

			if (directory_entry.is_directory()) /* 文件夹，递归目录 */
			{
                file_node->Type = FileType::Folder;
				file_node->Icon = TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH("EditorRes/icons/directory.png"));
				parent_node->ChildNodes.emplace_back(file_node);
				BuildFileNodeTree(file_node);
			}
			else
			{
				auto ext = filename.extension().string();
				transform(ext.begin(), ext.end(), ext.begin(), ::toupper);
				if (ext == ".JPG" || ext == ".PNG" || ext == ".DDS" || ext == ".TGA" || ext == ".BMP")
				{
                    file_node->Type = FileType::Image;

					auto filepath = std::filesystem::path(file_node->FilePath);
					auto relative_path = g_AssetsPath / filepath;
					file_node->Icon = TextureAssetManager::Instance().GetOrCreateTexture(relative_path.generic_string());
				}
				else if (ext == ".SCN")
				{
                    file_node->Type = FileType::Scene;
					file_node->Icon = TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH("EditorRes/icons/file_scn.png"));

				}
				else if (ext == ".MTLGRAPH")
				{
                    file_node->Type = FileType::MtlGraph;
					file_node->Icon = TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH("EditorRes/icons/file_mtlgraph.png"));
				}
				else
				{
                    file_node->Type = FileType::Default;
					file_node->Icon = TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH("EditorRes/icons/file.png"));
				}

				file_node->FileSize = static_cast<float>(std::filesystem::file_size(path)) / 1024.0f;
				parent_node->ChildNodes.emplace_back(file_node);
			}
		}
		m_IsDirty = false;
	}

	void EditorResourceBrowser::BuildFileUIListTreeDetail(const SharedPtr<FileNode>& node)
	{
		for (const auto& child_node : node->ChildNodes)
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();

			const auto file_ext = ExtractFileSuffix(child_node->FileName);

            if (child_node->Type == FileType::Folder) /* 文件夹 */
            {
                /* 文件夹节点 */
                bool open = ImGui::TreeNodeEx(child_node->FileName.c_str(), ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanFullWidth);

                /* 点击目录时，设置为当前文件目录 */
                /* 文件类型 */
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(100);
				ImGui::TextUnformatted(file_ext.c_str());

                /* 文件大小 */
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(300);
				ImGui::TextUnformatted("");

				/* 展开文件夹节点 */
				if (open)
				{
					BuildFileUIListTreeDetail(child_node);
					ImGui::TreePop();
				}
			}
			else
			{
                /* 文件节点 */
                ImGui::TreeNodeEx(child_node->FileName.c_str(), ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);

                /* todo: 点击文件时做响应 */
                if (ImGui::IsItemClicked() && ImGui::IsItemToggledOpen())
				{
					/**/
				}

				/* 右键选中菜单 */
				if (ImGui::BeginPopupContextItem())
				{
					/* 在系统文件夹中显示 */
					if (ImGui::MenuItem("Show in file explorer"))
					{
						auto path = g_AssetsPath / child_node->FilePath;
						path = absolute(path).make_preferred();
						OpenFileExplorer(path.string().c_str());
					}

					ImGui::EndPopup();
				}

				/* 文件类型 */
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(160);
				ImGui::TextUnformatted(file_ext.c_str());

				/* 文件大小 */
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(300);
				ImGui::TextUnformatted((ToString(child_node->FileSize, 2) + "KB").c_str());
			}
		}
	}

	void EditorResourceBrowser::BuildFileUIListTreeSimple(const SharedPtr<FileNode>& node)
	{
		for (const auto& child_node : node->ChildNodes)
		{
			if (child_node->Type == FileType::Folder) /* 文件夹 */
			{
				/* 文件夹节点 */
				bool open = ImGui::TreeNodeEx(child_node->FileName.c_str(), ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanFullWidth);

				/* 点击目录时，设置为当前文件目录 */
				if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
					m_CurrentFileNode = child_node;

				/* 展开文件夹节点 */
				if (open)
				{
					BuildFileUIListTreeSimple(child_node);
					ImGui::TreePop();
				}
			}
			else
			{
				/* 文件节点 */
				ImGui::TreeNodeEx(child_node->FileName.c_str(), ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);

				/* todo: 点击文件时做响应 */
				if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
				{
					/**/
				}
			}
		}
	}

	std::filesystem::path EditorResourceBrowser::GetRelativePath(const std::filesystem::path& dir, const std::filesystem::path& path)
	{
		return path.lexically_relative(dir);
	}
}
