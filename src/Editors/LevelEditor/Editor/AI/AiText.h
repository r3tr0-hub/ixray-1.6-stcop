#pragma once

namespace AiText
{
inline xr_string Utf8(const char* Value)
{
	if (Value == nullptr)
	{
		return {};
	}
	const xr_string Text = Value;
	return IsUTF8(Value) ? Text : Platform::ANSI_TO_UTF8(Text);
}
} // namespace AiText
