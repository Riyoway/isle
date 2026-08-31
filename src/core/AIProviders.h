#pragma once

#include <array>
#include <string_view>

namespace isle {

struct AIProviderInfo {
    std::wstring_view id;
    std::wstring_view name;
    std::wstring_view mark;
    std::wstring_view color;
};

inline constexpr std::array<AIProviderInfo, 56> kAIProviders{{
    {L"codex", L"Codex", L"◎", L"#64D2FF"},
    {L"claude", L"Claude", L"A", L"#D97757"},
    {L"cursor", L"Cursor", L"C", L"#FFFFFF"},
    {L"factory", L"Factory", L"F", L"#A78BFA"},
    {L"gemini", L"Gemini", L"✦", L"#4E8CFF"},
    {L"antigravity", L"Antigravity", L"AG", L"#BF5AF2"},
    {L"copilot", L"Copilot", L"∞", L"#34C759"},
    {L"zai", L"z.ai", L"Z", L"#5E5CE6"},
    {L"minimax", L"MiniMax", L"M", L"#FF375F"},
    {L"kiro", L"Kiro", L"K", L"#AF52DE"},
    {L"vertexai", L"Vertex AI", L"V", L"#4285F4"},
    {L"augment", L"Augment", L"A+", L"#30D158"},
    {L"opencode", L"OpenCode", L"OC", L"#FFD60A"},
    {L"kimi", L"Kimi", L"K", L"#0A84FF"},
    {L"kimik2", L"Kimi K2", L"K2", L"#40C8E0"},
    {L"amp", L"Amp", L"A", L"#FF9F0A"},
    {L"warp", L"Warp", L"W", L"#5856D6"},
    {L"ollama", L"Ollama", L"O", L"#F2F2F7"},
    {L"azureopenai", L"Azure OpenAI", L"AZ", L"#0078D4"},
    {L"t3chat", L"T3 Chat", L"T3", L"#FF453A"},
    {L"openrouter", L"OpenRouter", L"OR", L"#A1A1AA"},
    {L"jetbrains", L"JetBrains AI", L"JB", L"#FF2D55"},
    {L"alibaba", L"Alibaba", L"Ali", L"#FF6A00"},
    {L"alibabatokenplan", L"Alibaba Token Plan", L"AT", L"#FF9500"},
    {L"nanogpt", L"NanoGPT", L"N", L"#32D74B"},
    {L"infini", L"Infini", L"I", L"#5AC8FA"},
    {L"perplexity", L"Perplexity", L"P", L"#20C4C7"},
    {L"abacus", L"Abacus AI", L"A", L"#FFCC00"},
    {L"mistral", L"Mistral", L"M", L"#FF7A00"},
    {L"opencodego", L"OpenCode Go", L"OG", L"#30D158"},
    {L"kilo", L"Kilo", L"K", L"#FF6482"},
    {L"bedrock", L"AWS Bedrock", L"AWS", L"#FF9900"},
    {L"codebuff", L"Codebuff", L"CB", L"#63E6BE"},
    {L"deepseek", L"DeepSeek", L"DS", L"#4D6BFE"},
    {L"windsurf", L"Windsurf", L"WS", L"#00C2FF"},
    {L"manus", L"Manus", L"M", L"#F5F5F7"},
    {L"mimo", L"Xiaomi MiMo", L"Mi", L"#FF6900"},
    {L"doubao", L"Doubao", L"D", L"#3370FF"},
    {L"commandcode", L"Command Code", L"CC", L"#AC8E68"},
    {L"crof", L"Crof", L"Cr", L"#FFCC00"},
    {L"stepfun", L"StepFun", L"SF", L"#00C7BE"},
    {L"venice", L"Venice", L"V", L"#FF453A"},
    {L"openaiapi", L"OpenAI API", L"OA", L"#10A37F"},
    {L"grok", L"Grok", L"X", L"#F5F5F7"},
    {L"elevenlabs", L"ElevenLabs", L"11", L"#FFFFFF"},
    {L"deepgram", L"Deepgram", L"DG", L"#13EF93"},
    {L"groq", L"Groq", L"G", L"#F55036"},
    {L"llmproxy", L"LLM Proxy", L"LP", L"#8E8E93"},
    {L"chutes", L"Chutes", L"C", L"#FF9F0A"},
    {L"litellm", L"LiteLLM", L"LL", L"#5E5CE6"},
    {L"poe", L"Poe", L"P", L"#5D5CDE"},
    {L"devin", L"Devin", L"D", L"#30D158"},
    {L"zed", L"Zed", L"Z", L"#FF375F"},
    {L"crossmodel", L"CrossModel", L"CM", L"#64D2FF"},
    {L"qoder", L"Qoder", L"Q", L"#BF5AF2"},
    {L"sakana", L"Sakana AI", L"S", L"#FF6B6B"},
}};

inline int ai_provider_index(std::wstring_view id) noexcept {
    for (std::size_t i = 0; i < kAIProviders.size(); ++i) {
        if (kAIProviders[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

} // namespace isle
