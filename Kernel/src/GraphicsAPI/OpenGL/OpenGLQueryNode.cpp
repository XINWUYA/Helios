#include "Pch.h"
#include "OpenGLQueryNode.h"
#include <glad/glad.h>
#include "OpenGLCommon.h"

namespace Helios
{
	OpenGLQueryNode::OpenGLQueryNode()
	{
		glGenQueries(2, m_TimestampQueries);
		CHECK_GL_ERROR;
	}

	OpenGLQueryNode::~OpenGLQueryNode()
	{
		glDeleteQueries(2, m_TimestampQueries);
		CHECK_GL_ERROR;
	}

	void OpenGLQueryNode::Begin()
	{
		PROFILE_FUNCTION();

		CHECK_GL_ERROR;
		glQueryCounter(m_TimestampQueries[0], GL_TIMESTAMP);
		CHECK_GL_ERROR;
	}

	void OpenGLQueryNode::End()
	{
		PROFILE_FUNCTION();

		CHECK_GL_ERROR;
		glQueryCounter(m_TimestampQueries[1], GL_TIMESTAMP);
		CHECK_GL_ERROR;
	}

	bool OpenGLQueryNode::GetQueryResult()
	{
		PROFILE_FUNCTION();

		CHECK_GL_ERROR;
		uint64_t timestampAvailable[2] = {0};
		glGetQueryObjectui64v(m_TimestampQueries[0], GL_QUERY_RESULT_AVAILABLE, &timestampAvailable[0]);
		glGetQueryObjectui64v(m_TimestampQueries[1], GL_QUERY_RESULT_AVAILABLE, &timestampAvailable[1]);
		if (timestampAvailable[0] == GL_TRUE && timestampAvailable[1] == GL_TRUE)
		{
			glGetQueryObjectui64v(m_TimestampQueries[0], GL_QUERY_RESULT, &QueryTimeBegin);
			glGetQueryObjectui64v(m_TimestampQueries[1], GL_QUERY_RESULT, &QueryTimeEnd);

			ResultTimeBegin = static_cast<double>(QueryTimeBegin) * 1e-3; /* us */
			ResultTimeEnd= static_cast<double>(QueryTimeEnd) * 1e-3; /* us */

			/* GL 的时间戳查询与渲染通道无关（glQueryCounter 随处可用），
			 * 解析出结果即等于有效样本（与 Metal 的通道采样相对照） */
			HasValidSamples = true;
			return true;
		}
		CHECK_GL_ERROR;
		return false;
	}
}
