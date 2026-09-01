#include "AIUsageProvider.h"

#include "../core/AIProviders.h"
#include "../core/Settings.h"

#include <ShlObj.h>
#include <Windows.h>
#include <wincred.h>
#include <winhttp.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace isle {

using namespace winrt;
using namespace Windows::Data::Json;

namespace {

constexpr std::size_t kMaxResponseBytes = 2 * 1024 * 1024;

struct UsageWindow {
    double usedPercent{};
    std::int64_t resetsAt{};
};

struct CodexUsage {
    UsageWindow primary;
    std::optional<UsageWindow> secondary;
};

struct DirectWindow {
    std::wstring title;
    std::wstring subtitle;
    double usedPercent{};
};

struct DirectFetch {
    std::vector<DirectWindow> windows;
    std::wstring status;
};

enum class AuthKind {
    None,
    Bearer,
    ApiKey,
    Cookie,
};

struct EndpointSpec {
    const wchar_t* url;
    AuthKind auth;
    const wchar_t* header;
};

struct HttpResponse {
    DWORD status{};
    std::string body;
};

std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw))) return {};
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

std::filesystem::path codex_home() {
    std::array<wchar_t, 32768> configured{};
    const DWORD length = GetEnvironmentVariableW(L"CODEX_HOME", configured.data(),
                                                  static_cast<DWORD>(configured.size()));
    if (length > 0 && length < configured.size()) return configured.data();
    return known_folder(FOLDERID_Profile) / L".codex";
}

std::filesystem::path newest_session_file() {
    const auto root = codex_home() / L"sessions";
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return {};

    std::filesystem::path newest;
    std::filesystem::file_time_type newestTime{};
    for (std::filesystem::recursive_directory_iterator it(
             root, std::filesystem::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (!it->is_regular_file(ec) || it->path().extension() != L".jsonl") continue;
        const auto modified = it->last_write_time(ec);
        if (!ec && (newest.empty() || modified > newestTime)) {
            newest = it->path();
            newestTime = modified;
        }
    }
    return newest;
}

std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        result.data(), length);
    return result;
}

std::optional<std::string> read_file(const std::filesystem::path& path,
                                     std::size_t maxBytes = 4 * 1024 * 1024) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return std::nullopt;
    const auto length = input.tellg();
    if (length <= 0 || static_cast<std::uint64_t>(length) > maxBytes) return std::nullopt;
    input.seekg(0);
    std::string data(static_cast<std::size_t>(length), '\0');
    input.read(data.data(), length);
    data.resize(static_cast<std::size_t>(input.gcount()));
    return data;
}

std::wstring environment_value(std::wstring_view name) {
    if (name.empty()) return {};
    const std::wstring key(name);
    std::vector<wchar_t> buffer(256);
    for (;;) {
        const DWORD length = GetEnvironmentVariableW(key.c_str(), buffer.data(),
                                                      static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size()) return std::wstring(buffer.data(), length);
        buffer.resize(static_cast<std::size_t>(length) + 1);
    }
}

std::wstring lowercase_ascii(std::wstring_view value) {
    std::wstring result(value);
    for (wchar_t& ch : result) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return result;
}

std::wstring uppercase_ascii(std::wstring_view value) {
    std::wstring result(value);
    for (wchar_t& ch : result) {
        if (ch >= L'a' && ch <= L'z') ch = static_cast<wchar_t>(ch - L'a' + L'A');
        if (!(ch >= L'A' && ch <= L'Z') && !(ch >= L'0' && ch <= L'9')) ch = L'_';
    }
    return result;
}

std::wstring credential_value(std::wstring_view targetSuffix) {
    const std::wstring target = L"Isle/" + std::wstring(targetSuffix);
    CREDENTIALW* credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential) || !credential) return {};

    std::wstring result;
    if (credential->CredentialBlob && credential->CredentialBlobSize > 0) {
        const auto* bytes = reinterpret_cast<const char*>(credential->CredentialBlob);
        const std::string utf8(bytes, bytes + credential->CredentialBlobSize);
        result = utf8_to_wide(utf8);
        if (result.empty() && credential->CredentialBlobSize % sizeof(wchar_t) == 0) {
            const auto* wide = reinterpret_cast<const wchar_t*>(credential->CredentialBlob);
            result.assign(wide, wide + credential->CredentialBlobSize / sizeof(wchar_t));
        }
    }
    CredFree(credential);
    return result;
}

std::wstring first_environment(std::initializer_list<const wchar_t*> names) {
    for (const auto* name : names) {
        if (const auto value = environment_value(name); !value.empty()) return value;
    }
    return {};
}

std::wstring provider_token(std::wstring_view provider) {
    const std::wstring prefix = L"ISLE_" + uppercase_ascii(provider);
    if (const auto value = environment_value(prefix + L"_API_KEY"); !value.empty()) return value;
    if (const auto value = environment_value(prefix + L"_TOKEN"); !value.empty()) return value;

    if (provider == L"openrouter") return first_environment({L"OPENROUTER_API_KEY"});
    if (provider == L"openaiapi") return first_environment({L"OPENAI_ADMIN_KEY", L"OPENAI_ADMIN_API_KEY",
                                                              L"OPENAI_API_KEY", L"OPENAI_PLATFORM_API_KEY"});
    if (provider == L"deepseek") return first_environment({L"DEEPSEEK_API_KEY"});
    if (provider == L"zai") return first_environment({L"Z_AI_API_KEY", L"BIGMODEL_API_KEY",
                                                       L"ZHIPU_API_KEY", L"ZHIPUAI_API_KEY", L"GLM_API_KEY"});
    if (provider == L"gemini") return first_environment({L"GEMINI_API_KEY", L"GOOGLE_API_KEY"});
    if (provider == L"copilot") {
        if (const auto value = first_environment({L"GH_TOKEN", L"GITHUB_TOKEN"}); !value.empty()) return value;
        const auto home = known_folder(FOLDERID_Profile);
        for (const auto& path : {home / L".config" / L"gh" / L"hosts.yml",
                                 known_folder(FOLDERID_RoamingAppData) / L"GitHub CLI" / L"hosts.yml"}) {
            if (const auto data = read_file(path)) {
                const std::string marker = "oauth_token:";
                const auto at = data->find(marker);
                if (at != std::string::npos) {
                    auto token = data->substr(at + marker.size());
                    const auto end = token.find_first_of("\r\n #");
                    if (end != std::string::npos) token.resize(end);
                    if (const auto value = utf8_to_wide(token); !value.empty()) return value;
                }
            }
        }
    }
    if (provider == L"elevenlabs") return first_environment({L"ELEVENLABS_API_KEY"});
    if (provider == L"deepgram") return first_environment({L"DEEPGRAM_API_KEY"});
    if (provider == L"groq") return first_environment({L"GROQ_API_KEY"});
    if (provider == L"mistral") return first_environment({L"MISTRAL_API_KEY"});
    if (provider == L"minimax") return first_environment({L"MINIMAX_API_KEY"});
    if (provider == L"kimi") return first_environment({L"KIMI_CODE_API_KEY", L"KIMI_API_KEY",
                                                        L"MOONSHOT_API_KEY"});
    if (provider == L"kimik2") return first_environment({L"MOONSHOT_API_KEY", L"KIMI_API_KEY"});
    if (provider == L"venice") return first_environment({L"VENICE_API_KEY"});
    if (provider == L"poe") return first_environment({L"POE_API_KEY"});
    if (provider == L"nanogpt") return first_environment({L"NANOGPT_API_KEY"});
    if (provider == L"infini") return first_environment({L"INFINI_API_KEY"});
    if (provider == L"chutes") return first_environment({L"CHUTES_API_KEY"});
    if (provider == L"clinepass") return first_environment({L"CLINE_API_KEY", L"CLINEPASS_API_KEY"});
    if (provider == L"amp") return first_environment({L"AMP_API_KEY", L"SOURCEGRAPH_ACCESS_TOKEN"});
    if (provider == L"augment") return first_environment({L"AUGMENT_API_KEY"});
    if (provider == L"crof") return first_environment({L"CROF_API_KEY"});
    return credential_value(provider);
}

std::wstring provider_cookie(std::wstring_view provider) {
    const std::wstring prefix = L"ISLE_" + uppercase_ascii(provider);
    if (const auto value = environment_value(prefix + L"_COOKIE"); !value.empty()) return value;
    if (provider == L"claude") {
        if (const auto value = first_environment({L"CLAUDE_COOKIE", L"ANTHROPIC_COOKIE"}); !value.empty()) {
            return value;
        }
        if (const auto session = first_environment({L"CLAUDE_SESSION_KEY", L"ANTHROPIC_SESSION_KEY"});
            !session.empty()) {
            return L"sessionKey=" + session;
        }
    }
    return credential_value(std::wstring(provider) + L"/cookie");
}

std::wstring usage_url_override(std::wstring_view provider) {
    return environment_value(L"ISLE_" + uppercase_ascii(provider) + L"_USAGE_URL");
}

std::wstring connection_hint(std::wstring_view provider) {
    return L"Not connected · set ISLE_" + uppercase_ascii(provider) +
           L"_API_KEY or store a token in Isle/" + std::wstring(provider);
}

std::wstring reset_text(std::int64_t epochSeconds) {
    if (epochSeconds <= 0) return L"Live quota";
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const auto seconds = std::max<std::int64_t>(0, epochSeconds - static_cast<std::int64_t>(now));
    const auto minutes = (seconds + 59) / 60;
    if (minutes < 60) return L"Resets in " + std::to_wstring(minutes) + L"m";
    const auto hours = (minutes + 59) / 60;
    if (hours < 48) return L"Resets in " + std::to_wstring(hours) + L"h";
    return L"Resets in " + std::to_wstring((hours + 23) / 24) + L"d";
}

std::optional<CodexUsage> read_latest_codex_usage(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return std::nullopt;
    const auto length = input.tellg();
    if (length <= 0) return std::nullopt;

    constexpr std::streamoff maxTail = 1024 * 1024;
    const std::streamoff bytes = std::min<std::streamoff>(length, maxTail);
    input.seekg(length - bytes);
    std::string tail(static_cast<std::size_t>(bytes), '\0');
    input.read(tail.data(), bytes);
    tail.resize(static_cast<std::size_t>(input.gcount()));

    std::size_t match = tail.rfind("\"rate_limits\"");
    while (match != std::string::npos) {
        const auto lineStartMarker = tail.rfind('\n', match);
        const std::size_t lineStart = lineStartMarker == std::string::npos ? 0 : lineStartMarker + 1;
        const auto lineEndMarker = tail.find('\n', match);
        const std::size_t lineEnd = lineEndMarker == std::string::npos ? tail.size() : lineEndMarker;
        try {
            const auto root = JsonObject::Parse(hstring(utf8_to_wide(
                std::string_view(tail).substr(lineStart, lineEnd - lineStart))));
            const auto limits = root.GetNamedObject(L"payload").GetNamedObject(L"rate_limits");
            const auto primary = limits.GetNamedObject(L"primary");
            CodexUsage usage;
            usage.primary.usedPercent = std::clamp(primary.GetNamedNumber(L"used_percent"), 0.0, 100.0);
            usage.primary.resetsAt = static_cast<std::int64_t>(primary.GetNamedNumber(L"resets_at", 0));
            if (limits.HasKey(L"secondary") && limits.GetNamedValue(L"secondary").ValueType() == JsonValueType::Object) {
                const auto secondary = limits.GetNamedObject(L"secondary");
                usage.secondary = UsageWindow{
                    std::clamp(secondary.GetNamedNumber(L"used_percent"), 0.0, 100.0),
                    static_cast<std::int64_t>(secondary.GetNamedNumber(L"resets_at", 0)),
                };
            }
            return usage;
        } catch (...) {
            if (lineStart == 0) break;
            match = tail.rfind("\"rate_limits\"", lineStart - 1);
        }
    }
    return std::nullopt;
}

std::optional<HttpResponse> http_request(std::wstring_view url, std::wstring_view method,
                                         std::wstring headers, std::string_view body,
                                         int timeoutMs = 15000) {
    std::wstring urlCopy(url);
    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(urlCopy.data(), static_cast<DWORD>(urlCopy.size()), 0, &parts)) return std::nullopt;

    if (!parts.lpszHostName || parts.dwHostNameLength == 0) return std::nullopt;
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path;
    if (parts.lpszUrlPath && parts.dwUrlPathLength > 0) path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    const std::wstring methodName(method);
    if (parts.lpszExtraInfo && parts.dwExtraInfoLength > 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    const bool secure = parts.lpszScheme &&
                        lowercase_ascii(std::wstring_view(parts.lpszScheme, parts.dwSchemeLength)) == L"https";

    HINTERNET session = WinHttpOpen(L"Isle/0.2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return std::nullopt;
    WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    HINTERNET connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        return std::nullopt;
    }
    HINTERNET request = WinHttpOpenRequest(connection, methodName.c_str(), path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    // Never forward bearer/cookie headers across an untrusted redirect.
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    if (!headers.empty() && !headers.ends_with(L"\r\n")) headers += L"\r\n";
    const void* optional = body.empty() ? nullptr : body.data();
    const BOOL sent = WinHttpSendRequest(
        request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : static_cast<DWORD>(headers.size()),
        const_cast<void*>(optional), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);

    std::string output;
    std::array<char, 8192> buffer{};
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
        const DWORD toRead = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), toRead, &read) || read == 0) break;
        if (output.size() + read > kMaxResponseBytes) {
            output.clear();
            break;
        }
        output.append(buffer.data(), read);
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (output.empty() && status == 0) return std::nullopt;
    return HttpResponse{status, std::move(output)};
}

std::optional<HttpResponse> http_get(std::wstring_view url, std::wstring headers = {}) {
    return http_request(url, L"GET", std::move(headers), {});
}

std::optional<HttpResponse> http_post_json(std::wstring_view url, std::wstring headers,
                                           std::string_view body) {
    headers += L"Content-Type: application/json\r\n";
    return http_request(url, L"POST", std::move(headers), body);
}

std::optional<IJsonValue> parse_json(std::string_view body) {
    try {
        auto text = utf8_to_wide(body);
        if (!text.empty() && text.front() == 0xFEFF) text.erase(text.begin());
        return JsonValue::Parse(hstring(text));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<double> number_value(const IJsonValue& value) {
    try {
        if (value.ValueType() == JsonValueType::Number) return value.GetNumber();
        if (value.ValueType() == JsonValueType::String) {
            const auto text = std::wstring(value.GetString().c_str());
            std::size_t consumed = 0;
            const double number = std::stod(text, &consumed);
            if (consumed == text.size()) return number;
        }
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<std::wstring> string_value(const IJsonValue& value) {
    try {
        if (value.ValueType() == JsonValueType::String) return std::wstring(value.GetString().c_str());
    } catch (...) {
    }
    return std::nullopt;
}

bool key_contains(std::wstring_view key, std::wstring_view fragment) {
    return key.find(fragment) != std::wstring_view::npos;
}

std::wstring object_text(const JsonObject& object, std::initializer_list<std::wstring_view> names) {
    for (const auto& property : object) {
        const auto key = lowercase_ascii(property.Key().c_str());
        for (const auto name : names) {
            if (key == name) {
                if (const auto text = string_value(property.Value()); text && !text->empty()) return *text;
            }
        }
    }
    return {};
}

std::wstring path_title(std::wstring_view path) {
    if (path.empty()) return L"Usage";
    const auto slash = path.rfind(L'/');
    std::wstring title(path.substr(slash == std::wstring_view::npos ? 0 : slash + 1));
    for (wchar_t& ch : title) {
        if (ch == L'_' || ch == L'-') ch = L' ';
    }
    if (title == L"five hour") return L"5 hours";
    if (title == L"seven day" || title == L"weekly all") return L"1 week";
    if (title == L"one day") return L"Daily";
    if (!title.empty() && title.front() >= L'a' && title.front() <= L'z') {
        title.front() = static_cast<wchar_t>(title.front() - L'a' + L'A');
    }
    return title.empty() ? L"Usage" : title;
}

std::wstring reset_description(const JsonObject& object) {
    for (const auto& property : object) {
        const auto key = lowercase_ascii(property.Key().c_str());
        if (key != L"reset_description" && key != L"resetdescription" && key != L"resets_at" &&
            key != L"resetat" && key != L"reset") continue;
        if (const auto text = string_value(property.Value()); text && !text->empty()) return *text;
        if (const auto number = number_value(property.Value())) return reset_text(static_cast<std::int64_t>(*number));
    }
    return {};
}

std::optional<double> explicit_percent(const JsonObject& object) {
    struct Candidate {
        int rank;
        double value;
    };
    std::optional<Candidate> best;
    for (const auto& property : object) {
        const auto key = lowercase_ascii(property.Key().c_str());
        const auto value = number_value(property.Value());
        if (!value) continue;
        int rank = -1;
        if (key == L"used_percent" || key == L"percent_used" || key == L"total_percent_used" ||
            key == L"usage_percent" || key == L"usagepercentage") rank = 0;
        else if (key == L"utilization" || key == L"percentage" || key == L"percent") rank = 1;
        else if (key_contains(key, L"remaining_fraction")) rank = 2;
        else if (key_contains(key, L"remaining_percent")) rank = 3;
        if (rank < 0) continue;
        double normalized = *value;
        if (rank == 2) normalized = (1.0 - normalized) * 100.0;
        else if (rank == 3) normalized = 100.0 - normalized;
        if (rank <= 1 && key_contains(key, L"fraction")) normalized *= 100.0;
        if (!best || rank < best->rank) best = Candidate{rank, normalized};
    }
    return best ? std::optional<double>(std::clamp(best->value, 0.0, 100.0)) : std::nullopt;
}

std::optional<double> ratio_percent(const JsonObject& object) {
    std::optional<double> used;
    std::optional<double> limit;
    std::optional<double> remaining;
    for (const auto& property : object) {
        const auto key = lowercase_ascii(property.Key().c_str());
        const auto value = number_value(property.Value());
        if (!value) continue;
        if (key == L"used" || key == L"usage" || key == L"consumed" || key == L"current" ||
            key == L"currentvalue" || key == L"total_usage" || key == L"used_credits" ||
            key == L"credits_used" || key == L"used_balance" || key == L"character_count" ||
            key == L"current_usage" || key == L"currentusage" || key == L"tokens_used" ||
            key == L"used_tokens") {
            if (!used || key == L"currentvalue" || key == L"used") used = *value;
        } else if (key == L"limit" || key == L"total" || key == L"quota" || key == L"capacity" ||
                   key == L"max" || key == L"total_credits" || key == L"total_granted" ||
                   key == L"entitlement" || key == L"monthly_credit_limit" || key == L"token_limit" ||
                   key == L"credits_limit" || key == L"total_balance" || key == L"total_quota" ||
                   key == L"quota_limit" || key == L"character_limit" || key == L"usage_limit" ||
                   key == L"monthly_limit" || key == L"requests_limit") {
            if (!limit || key == L"limit" || key == L"total_credits" || key == L"total_granted") limit = *value;
        } else if (key == L"remaining" || key == L"credits_remaining" || key == L"tokens_remaining") {
            remaining = *value;
        }
    }
    if (limit && *limit > 0.0) {
        const double consumed = used.value_or(remaining ? *limit - *remaining : 0.0);
        return std::clamp(consumed / *limit * 100.0, 0.0, 100.0);
    }
    return std::nullopt;
}

std::optional<DirectWindow> metric_from_object(const JsonObject& object, std::wstring_view path) {
    const auto percent = explicit_percent(object).value_or(ratio_percent(object).value_or(-1.0));
    if (percent < 0.0) return std::nullopt;
    auto title = object_text(object, {L"title", L"label", L"name", L"model", L"model_id", L"window", L"period", L"plan"});
    if (title.empty()) title = path_title(path);
    if (title.size() > 48) title.resize(48);
    return DirectWindow{std::move(title), reset_description(object), percent};
}

void collect_windows(const IJsonValue& value, std::wstring path, std::vector<DirectWindow>& output, int depth = 0) {
    if (depth > 8 || output.size() >= 4) return;
    try {
        if (value.ValueType() == JsonValueType::Object) {
            const auto object = value.GetObject();
            if (const auto metric = metric_from_object(object, path)) output.push_back(*metric);
            for (const auto& property : object) {
                if (property.Value().ValueType() != JsonValueType::Object &&
                    property.Value().ValueType() != JsonValueType::Array) continue;
                const auto child = path.empty() ? std::wstring(property.Key().c_str())
                                                : path + L"/" + property.Key().c_str();
                collect_windows(property.Value(), child, output, depth + 1);
                if (output.size() >= 4) break;
            }
        } else if (value.ValueType() == JsonValueType::Array) {
            int index = 0;
            for (const auto& item : value.GetArray()) {
                collect_windows(item, path + L"/" + std::to_wstring(index++), output, depth + 1);
                if (output.size() >= 4) break;
            }
        }
    } catch (...) {
    }
}

std::vector<DirectWindow> extract_windows(const IJsonValue& root) {
    std::vector<DirectWindow> windows;
    collect_windows(root, {}, windows);
    std::vector<DirectWindow> unique;
    for (auto& window : windows) {
        const bool duplicate = std::any_of(unique.begin(), unique.end(), [&](const DirectWindow& other) {
            return other.title == window.title && std::abs(other.usedPercent - window.usedPercent) < 0.001;
        });
        if (!duplicate) unique.push_back(std::move(window));
    }
    return unique;
}

std::vector<DirectWindow> extract_claude_windows(const IJsonValue& root) {
    if (root.ValueType() != JsonValueType::Object) return extract_windows(root);

    constexpr std::array<std::pair<std::wstring_view, std::wstring_view>, 6> fields{{
        {L"five_hour", L"5 hours"},
        {L"seven_day", L"1 week"},
        {L"seven_day_oauth_apps", L"1 week · OAuth"},
        {L"seven_day_opus", L"1 week · Opus"},
        {L"seven_day_sonnet", L"1 week · Sonnet"},
        {L"seven_day_cowork", L"1 week · Cowork"},
    }};

    std::vector<DirectWindow> windows;
    try {
        const auto object = root.GetObject();
        for (const auto& [key, title] : fields) {
            if (!object.HasKey(key.data())) continue;
            const auto value = object.GetNamedValue(key.data());
            if (value.ValueType() != JsonValueType::Object) continue;
            const auto usage = value.GetObject();
            const double percent = explicit_percent(usage).value_or(ratio_percent(usage).value_or(-1.0));
            if (percent < 0.0) continue;
            windows.push_back({std::wstring(title), reset_description(usage), percent});
            if (windows.size() == 4) break;
        }
    } catch (...) {
        return {};
    }
    return windows.empty() ? extract_windows(root) : windows;
}

std::wstring first_string_for_key(const IJsonValue& value, std::wstring_view wanted, int depth = 0) {
    if (depth > 8) return {};
    try {
        if (value.ValueType() == JsonValueType::Object) {
            for (const auto& property : value.GetObject()) {
                if (lowercase_ascii(property.Key().c_str()) == wanted) {
                    if (const auto text = string_value(property.Value()); text) return *text;
                }
                const auto nested = first_string_for_key(property.Value(), wanted, depth + 1);
                if (!nested.empty()) return nested;
            }
        } else if (value.ValueType() == JsonValueType::Array) {
            for (const auto& item : value.GetArray()) {
                const auto nested = first_string_for_key(item, wanted, depth + 1);
                if (!nested.empty()) return nested;
            }
        }
    } catch (...) {
    }
    return {};
}

EndpointSpec endpoint_for(std::wstring_view provider) {
    if (provider == L"openrouter") return {L"https://openrouter.ai/api/v1/credits", AuthKind::Bearer, L""};
    if (provider == L"openaiapi") return {L"https://api.openai.com/v1/dashboard/billing/credit_grants", AuthKind::Bearer, L""};
    if (provider == L"deepseek") return {L"https://api.deepseek.com/user/balance", AuthKind::Bearer, L""};
    if (provider == L"zai") return {L"https://api.z.ai/api/monitor/usage/quota/limit", AuthKind::Bearer, L""};
    if (provider == L"minimax") return {L"https://platform.minimax.io/v1/api/openplatform/coding_plan/remains", AuthKind::Bearer, L""};
    if (provider == L"kimi") return {L"https://api.kimi.com/coding/v1/usages", AuthKind::Bearer, L""};
    if (provider == L"kimik2") return {L"https://api.moonshot.ai/v1/users/me/balance", AuthKind::Bearer, L""};
    if (provider == L"chutes") return {L"https://api.chutes.ai/users/me/subscription_usage", AuthKind::Bearer, L""};
    if (provider == L"nanogpt") return {L"https://nano-gpt.com/api/subscription/v1/usage", AuthKind::Bearer, L""};
    if (provider == L"elevenlabs") return {L"https://api.elevenlabs.io/v1/user/subscription", AuthKind::ApiKey, L"xi-api-key"};
    if (provider == L"poe") return {L"https://api.poe.com/usage/current_balance", AuthKind::Bearer, L""};
    if (provider == L"venice") return {L"https://api.venice.ai/api/v1/billing/balance", AuthKind::Bearer, L""};
    if (provider == L"deepinfra") return {L"https://api.deepinfra.com/payment/usage?from=current", AuthKind::Bearer, L""};
    if (provider == L"clinepass") return {L"https://api.cline.bot/api/v1/users/me/plan/usage-limits", AuthKind::Bearer, L""};
    if (provider == L"amp") return {L"https://sourcegraph.com/.api/cody/current-user/usage", AuthKind::Bearer, L""};
    if (provider == L"augment") return {L"https://api.augmentcode.com/v1/user/usage", AuthKind::Bearer, L""};
    if (provider == L"crof") return {L"https://crof.ai/usage_api/", AuthKind::Bearer, L""};
    if (provider == L"opencodego") return {L"https://opencode.ai/zen/go/v1/usage", AuthKind::Cookie, L""};
    if (provider == L"ollama") return {L"http://127.0.0.1:11434/api/tags", AuthKind::None, L""};
    return {nullptr, AuthKind::None, L""};
}

DirectFetch fetch_endpoint(std::wstring_view provider, const EndpointSpec& spec,
                           std::wstring_view token, std::wstring_view cookie) {
    if (spec.auth == AuthKind::Bearer || spec.auth == AuthKind::ApiKey) {
        if (token.empty()) return {{}, connection_hint(provider)};
    } else if (spec.auth == AuthKind::Cookie && cookie.empty()) {
        return {{}, L"Not connected · set ISLE_" + uppercase_ascii(provider) + L"_COOKIE"};
    }

    std::wstring headers = L"Accept: application/json\r\n";
    if (spec.auth == AuthKind::Bearer) headers += L"Authorization: Bearer " + std::wstring(token) + L"\r\n";
    else if (spec.auth == AuthKind::ApiKey) headers += std::wstring(spec.header) + L": " + std::wstring(token) + L"\r\n";
    else if (spec.auth == AuthKind::Cookie) headers += L"Cookie: " + std::wstring(cookie) + L"\r\n";
    const auto response = http_get(spec.url, std::move(headers));
    if (!response) return {{}, L"Unable to reach provider"};
    if (response->status == 401 || response->status == 403) return {{}, L"Authentication required"};
    if (response->status < 200 || response->status >= 300) {
        return {{}, L"Provider returned HTTP " + std::to_wstring(response->status)};
    }
    const auto root = parse_json(response->body);
    if (!root) return {{}, L"Provider returned invalid JSON"};
    auto windows = extract_windows(*root);
    if (!windows.empty()) return {std::move(windows), {}};
    if (provider == L"ollama") {
        int modelCount = 0;
        try {
            if (root->ValueType() == JsonValueType::Object && root->GetObject().HasKey(L"models")) {
                modelCount = static_cast<int>(root->GetObject().GetNamedArray(L"models").Size());
            }
        } catch (...) {
        }
        return {{}, L"Connected · " + std::to_wstring(modelCount) + L" local models"};
    }
    if (!first_string_for_key(*root, L"error").empty()) return {{}, L"Provider returned an error"};
    return {{}, L"Connected · no usage window returned"};
}

DirectFetch fetch_claude() {
    if (const auto credentials = read_file(known_folder(FOLDERID_Profile) / L".claude" / L".credentials.json")) {
        if (const auto root = parse_json(*credentials)) {
            const auto accessToken = first_string_for_key(*root, L"accesstoken");
            if (!accessToken.empty()) {
                const auto response = http_get(
                    L"https://api.anthropic.com/api/oauth/usage",
                    L"Accept: application/json\r\nAuthorization: Bearer " + accessToken +
                    L"\r\nanthropic-beta: oauth-2025-04-20\r\n");
                if (response && response->status >= 200 && response->status < 300) {
                    if (const auto oauthUsageRoot = parse_json(response->body)) {
                        if (auto windows = extract_claude_windows(*oauthUsageRoot); !windows.empty()) {
                            return {std::move(windows), {}};
                        }
                    }
                }
            }
        }
    }
    const auto cookie = provider_cookie(L"claude");
    if (cookie.empty()) return {{}, connection_hint(L"claude")};
    const std::wstring headers = L"Accept: application/json\r\nCookie: " + cookie +
                                 L"\r\nOrigin: https://claude.ai\r\nReferer: https://claude.ai/settings/usage\r\n";
    const auto organizations = http_get(L"https://claude.ai/api/organizations", headers);
    if (!organizations || organizations->status == 401 || organizations->status == 403) return {{}, L"Authentication required"};
    if (organizations->status < 200 || organizations->status >= 300) return {{}, L"Unable to load Claude account"};
    const auto orgRoot = parse_json(organizations->body);
    if (!orgRoot) return {{}, L"Claude returned invalid JSON"};
    const auto organizationId = first_string_for_key(*orgRoot, L"uuid");
    if (organizationId.empty()) return {{}, L"Claude organization not found"};

    const auto usage = http_get(L"https://claude.ai/api/organizations/" + organizationId + L"/usage", headers);
    if (!usage || usage->status == 401 || usage->status == 403) return {{}, L"Authentication required"};
    if (usage->status < 200 || usage->status >= 300) return {{}, L"Unable to load Claude usage"};
    const auto usageRoot = parse_json(usage->body);
    if (!usageRoot) return {{}, L"Claude returned invalid usage JSON"};
    auto windows = extract_claude_windows(*usageRoot);
    if (windows.empty()) return {{}, L"Connected · no usage window returned"};
    return {std::move(windows), {}};
}

DirectFetch fetch_gemini() {
    const auto credentials = read_file(known_folder(FOLDERID_Profile) / L".gemini" / L"oauth_creds.json");
    if (!credentials) return {{}, L"Not connected · run Gemini CLI login"};
    const auto root = parse_json(*credentials);
    if (!root || root->ValueType() != JsonValueType::Object) return {{}, L"Gemini credentials are invalid"};
    const auto accessToken = first_string_for_key(*root, L"access_token");
    if (accessToken.empty()) return {{}, L"Gemini access token not found"};
    const std::wstring headers = L"Accept: application/json\r\nAuthorization: Bearer " + accessToken + L"\r\n";
    const auto response = http_post_json(L"https://cloudcode-pa.googleapis.com/v1internal:retrieveUserQuota",
                                         headers, "{}");
    if (!response || response->status == 401 || response->status == 403) return {{}, L"Authentication required"};
    if (response->status < 200 || response->status >= 300) return {{}, L"Unable to load Gemini quota"};
    const auto quota = parse_json(response->body);
    if (!quota) return {{}, L"Gemini returned invalid quota JSON"};
    auto windows = extract_windows(*quota);
    if (windows.empty()) return {{}, L"Connected · no usage window returned"};
    return {std::move(windows), {}};
}

DirectFetch fetch_cursor() {
    const auto cookie = provider_cookie(L"cursor");
    if (cookie.empty()) return {{}, L"Not connected · set ISLE_CURSOR_COOKIE"};
    const auto response = http_get(L"https://cursor.com/api/usage-summary",
                                   L"Accept: application/json\r\nCookie: " + cookie + L"\r\n");
    if (!response || response->status == 401 || response->status == 403) return {{}, L"Authentication required"};
    if (response->status < 200 || response->status >= 300) return {{}, L"Unable to load Cursor usage"};
    const auto root = parse_json(response->body);
    if (!root) return {{}, L"Cursor returned invalid usage JSON"};
    auto windows = extract_windows(*root);
    if (windows.empty()) return {{}, L"Connected · no usage window returned"};
    return {std::move(windows), {}};
}

DirectFetch fetch_copilot() {
    const auto token = provider_token(L"copilot");
    if (token.empty()) return {{}, L"Not connected · set GH_TOKEN or GITHUB_TOKEN"};
    const auto response = http_get(L"https://api.github.com/copilot_internal/user",
                                   L"Accept: application/json\r\nAuthorization: Bearer " + token +
                                   L"\r\nUser-Agent: Isle\r\n");
    if (!response || response->status == 401 || response->status == 403) return {{}, L"Authentication required"};
    if (response->status < 200 || response->status >= 300) return {{}, L"Unable to load Copilot usage"};
    const auto root = parse_json(response->body);
    if (!root) return {{}, L"Copilot returned invalid usage JSON"};
    auto windows = extract_windows(*root);
    if (windows.empty()) return {{}, L"Connected · no usage window returned"};
    return {std::move(windows), {}};
}

DirectFetch fetch_direct(std::wstring_view provider) {
    if (provider == L"claude") return fetch_claude();
    if (provider == L"gemini") return fetch_gemini();
    if (provider == L"cursor") return fetch_cursor();
    if (provider == L"copilot") return fetch_copilot();

    const auto overrideUrl = usage_url_override(provider);
    const auto token = provider_token(provider);
    const auto cookie = provider_cookie(provider);
    if (!overrideUrl.empty()) {
        const AuthKind auth = !cookie.empty() ? AuthKind::Cookie : !token.empty() ? AuthKind::Bearer : AuthKind::None;
        return fetch_endpoint(provider, EndpointSpec{overrideUrl.c_str(), auth, L""}, token, cookie);
    }

    const auto spec = endpoint_for(provider);
    if (!spec.url) return {{}, L"No direct adapter · set ISLE_" + uppercase_ascii(provider) + L"_USAGE_URL"};
    return fetch_endpoint(provider, spec, token, cookie);
}

Activity usage_activity(std::wstring_view provider, std::wstring_view key, std::wstring title,
                        double usedPercent, std::wstring subtitle, const Settings& settings) {
    const int providerIndex = ai_provider_index(provider);
    const auto& info = providerIndex >= 0 ? kAIProviders[static_cast<std::size_t>(providerIndex)] : kAIProviders[0];
    Activity activity;
    activity.id = L"ai." + std::wstring(provider) + L"." + std::wstring(key);
    activity.source = L"ai." + std::wstring(provider);
    activity.kind = ActivityKind::Metric;
    activity.title = std::move(title);
    activity.subtitle = subtitle.empty() ? L"Live quota" : std::move(subtitle);
    activity.glyph = info.mark;
    activity.accent = providerIndex >= 0 ? settings.aiColors[static_cast<std::size_t>(providerIndex)]
                                         : std::wstring(info.color);
    activity.value = std::clamp(usedPercent, 0.0, 100.0);
    activity.valueSuffix = L"%";
    activity.progress = *activity.value / 100.0;
    const int windowPriority = key == L"primary" ? 4 : key == L"secondary" ? 3 : key == L"model_specific" ? 2 : 1;
    activity.priority = 320 - std::max(providerIndex, 0) + windowPriority;
    activity.pinned = true;
    activity.compactRing = providerIndex >= 0 && settings.aiRings[static_cast<std::size_t>(providerIndex)];
    activity.actions = {{L"refresh", L"Refresh", L"\uE72C"}};
    return activity;
}

Activity status_activity(std::wstring_view provider, std::wstring message, const Settings& settings) {
    const int providerIndex = ai_provider_index(provider);
    const auto& info = providerIndex >= 0 ? kAIProviders[static_cast<std::size_t>(providerIndex)] : kAIProviders[0];
    if (message.size() > 96) message.resize(96);
    Activity activity;
    activity.id = L"ai." + std::wstring(provider) + L".status";
    activity.source = L"ai." + std::wstring(provider);
    activity.kind = ActivityKind::Status;
    activity.title = std::wstring(info.name);
    activity.subtitle = message.empty() ? L"Not connected" : std::move(message);
    activity.glyph = info.mark;
    activity.accent = providerIndex >= 0 ? settings.aiColors[static_cast<std::size_t>(providerIndex)]
                                         : std::wstring(info.color);
    activity.priority = 250 - std::max(providerIndex, 0);
    activity.actions = {{L"refresh", L"Refresh", L"\uE72C"}};
    return activity;
}

void publish_direct_result(ActivityStore& store, const Settings& settings,
                           std::wstring_view provider, DirectFetch result) {
    const int providerIndex = ai_provider_index(provider);
    if (providerIndex < 0 || !settings.aiVisible[static_cast<std::size_t>(providerIndex)]) return;
    store.remove_source(L"ai." + std::wstring(provider));
    if (result.windows.empty()) {
        store.upsert(status_activity(provider, std::move(result.status), settings));
        return;
    }
    constexpr std::wstring_view keys[]{L"primary", L"secondary", L"model_specific", L"tertiary"};
    for (std::size_t i = 0; i < result.windows.size() && i < std::size(keys); ++i) {
        auto& window = result.windows[i];
        store.upsert(usage_activity(provider, keys[i], std::move(window.title), window.usedPercent,
                                    std::move(window.subtitle), settings));
    }
}

} // namespace

void AIUsageProvider::start(ActivityStore& store) {
    store_ = &store;
    lastUpdate_ = {};
    publish();
    refreshThread_ = std::jthread([this](std::stop_token stopToken) { direct_loop(stopToken); });
}

void AIUsageProvider::stop() {
    if (refreshThread_.joinable()) {
        refreshThread_.request_stop();
        refreshThread_.join();
    }
    if (store_) {
        for (const auto& provider : kAIProviders) store_->remove_source(L"ai." + std::wstring(provider.id));
    }
    store_ = nullptr;
    hasUsage_ = false;
}

void AIUsageProvider::tick() {
    if (!store_) return;
    const auto now = std::chrono::steady_clock::now();
    if (lastUpdate_.time_since_epoch().count() != 0 && now - lastUpdate_ < std::chrono::seconds(12)) return;
    publish();
}

void AIUsageProvider::invoke(std::wstring_view activityId, std::wstring_view actionId) {
    if (activityId.starts_with(L"ai.") && actionId == L"refresh") {
        publish();
        refreshRequested_ = true;
    }
}

void AIUsageProvider::publish() {
    if (!store_) return;
    lastUpdate_ = std::chrono::steady_clock::now();
    const Settings settings = Settings::load();
    for (std::size_t i = 0; i < kAIProviders.size(); ++i) {
        if (!settings.aiVisible[i]) store_->remove_source(L"ai." + std::wstring(kAIProviders[i].id));
    }

    const auto usage = read_latest_codex_usage(newest_session_file());
    if (usage && settings.aiVisible[0]) {
        hasUsage_ = true;
        store_->remove_source(L"ai.codex");
        store_->upsert(usage_activity(L"codex", L"primary", L"5 hours", usage->primary.usedPercent,
                                      reset_text(usage->primary.resetsAt), settings));
        if (usage->secondary) {
            store_->upsert(usage_activity(L"codex", L"secondary", L"Weekly", usage->secondary->usedPercent,
                                          reset_text(usage->secondary->resetsAt), settings));
        }
    } else if (settings.aiVisible[0] && !hasUsage_) {
        Activity waiting;
        waiting.id = L"ai.codex.waiting";
        waiting.source = L"ai.codex";
        waiting.kind = ActivityKind::Status;
        waiting.title = L"Codex";
        waiting.subtitle = L"Waiting for local usage data";
        waiting.glyph = kAIProviders[0].mark;
        waiting.accent = settings.aiColors[0];
        waiting.priority = 220;
        waiting.actions = {{L"refresh", L"Refresh", L"\uE72C"}};
        store_->upsert(std::move(waiting));
    }
}

void AIUsageProvider::direct_loop(std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
        refreshRequested_ = false;
        const Settings settings = Settings::load();
        if (store_) {
            for (std::size_t i = 0; i < kAIProviders.size(); ++i) {
                if (stopToken.stop_requested() || !store_) break;
                if (!settings.aiVisible[i] || kAIProviders[i].id == L"codex") continue;
                const auto provider = kAIProviders[i].id;
                publish_direct_result(*store_, settings, provider, fetch_direct(provider));
            }
        }
        for (int second = 0; second < 300 && !stopToken.stop_requested(); ++second) {
            if (refreshRequested_.exchange(false)) break;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

} // namespace isle
