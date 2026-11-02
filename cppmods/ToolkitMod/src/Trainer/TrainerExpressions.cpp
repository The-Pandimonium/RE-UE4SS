#include <Trainer/TrainerEngine.hpp>

#include <algorithm>
#include <charconv>
#include <format>

#include <EngineCompat/GameThread.hpp>
#include <EngineCompat/Reflect.hpp>
#include <Helpers/String.hpp>
#include <Trainer/TargetResolver.hpp>

// Placeholders and arithmetic for trainer values. See TrainerModel.hpp for the syntax.

namespace RC::Toolkit
{
    using namespace RC::Unreal;
    namespace Reflect = RC::Compat::Reflect;
    using RC::Compat::GameThread;

    namespace
    {
        auto number_text(double v) -> std::string
        {
            return std::format("{}", v);
        }

        // Recursive-descent evaluator for + - * / ( ) and unary minus over plain numbers.
        struct Parser
        {
            const std::string& s;
            size_t i{0};

            void skip() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i; }

            auto expr() -> std::optional<double>
            {
                auto value = term();
                if (!value) return std::nullopt;
                for (;;)
                {
                    skip();
                    if (i < s.size() && (s[i] == '+' || s[i] == '-'))
                    {
                        const char op = s[i++];
                        auto rhs = term();
                        if (!rhs) return std::nullopt;
                        value = op == '+' ? *value + *rhs : *value - *rhs;
                    }
                    else break;
                }
                return value;
            }

            auto term() -> std::optional<double>
            {
                auto value = factor();
                if (!value) return std::nullopt;
                for (;;)
                {
                    skip();
                    if (i < s.size() && (s[i] == '*' || s[i] == '/'))
                    {
                        const char op = s[i++];
                        auto rhs = factor();
                        if (!rhs) return std::nullopt;
                        if (op == '/' && *rhs == 0.0) return std::nullopt;
                        value = op == '*' ? *value * *rhs : *value / *rhs;
                    }
                    else break;
                }
                return value;
            }

            auto factor() -> std::optional<double>
            {
                skip();
                if (i >= s.size()) return std::nullopt;
                if (s[i] == '-')
                {
                    ++i;
                    auto v = factor();
                    return v ? std::optional<double>(-*v) : std::nullopt;
                }
                if (s[i] == '+')
                {
                    ++i;
                    return factor();
                }
                if (s[i] == '(')
                {
                    ++i;
                    auto v = expr();
                    skip();
                    if (!v || i >= s.size() || s[i] != ')') return std::nullopt;
                    ++i;
                    return v;
                }
                double d = 0.0;
                auto [ptr, ec] = std::from_chars(s.data() + i, s.data() + s.size(), d);
                if (ec != std::errc{}) return std::nullopt;
                i = static_cast<size_t>(ptr - s.data());
                return d;
            }
        };

        // Text form of a property's value: numbers (and bools/enums) as numbers, everything else via the engine's form.
        auto read_property_text(UObject* object, const std::string& property) -> std::optional<std::string>
        {
            if (!object) return std::nullopt;
            FProperty* p = Reflect::FindProperty(object, ensure_str(property));
            if (!p) return std::nullopt;
            void* ptr = p->ContainerPtrToValuePtr<void>(object);
            if (auto n = Reflect::GetNumber(p, ptr)) return number_text(*n);
            if (auto text = Reflect::ExportText(p, ptr, object)) return to_string(*text);
            return std::nullopt;
        }

        // "{class:daynightCycle_C.MaxTime}" -> value of MaxTime on the first daynightCycle_C.
        auto read_reference(std::string inner) -> std::optional<std::string>
        {
            inner.erase(std::remove(inner.begin(), inner.end(), '"'), inner.end());
            const auto first = inner.find_first_not_of(' ');
            if (first == std::string::npos) return std::nullopt;
            inner = inner.substr(first, inner.find_last_not_of(' ') - first + 1);
            const auto dot = inner.rfind('.');
            if (dot == std::string::npos || dot == 0 || dot + 1 >= inner.size()) return std::nullopt;
            auto objects = resolve_targets(inner.substr(0, dot), "first", 0);
            if (objects.empty()) return std::nullopt;
            return read_property_text(objects.front(), inner.substr(dot + 1));
        }

        auto live_current(const ActionDef* action) -> std::optional<std::string>
        {
            if (!action || action->property.empty()) return std::nullopt;
            auto objects = resolve_targets(action->target, action->select, action->index);
            if (objects.empty()) return std::nullopt;
            return read_property_text(objects.front(), action->property);
        }
    } // namespace

    auto evaluate_expression(const std::string& text) -> std::optional<double>
    {
        std::string t = text;
        std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (t == "true") return 1.0;
        if (t == "false") return 0.0;
        Parser parser{text};
        auto value = parser.expr();
        parser.skip();
        if (!value || parser.i != text.size()) return std::nullopt;
        return value;
    }

    auto TrainerEngine::expand_text(const std::string& text, double slider, const ActionDef* action, const std::optional<std::string>& current_text) -> std::string
    {
        std::string out;
        size_t i = 0;
        while (i < text.size())
        {
            if (text[i] != '{')
            {
                out += text[i++];
                continue;
            }
            const auto close = text.find('}', i);
            if (close == std::string::npos)
            {
                out += text.substr(i);
                break;
            }
            const std::string inner = text.substr(i + 1, close - i - 1);
            std::optional<std::string> replacement;
            if (inner == "value") replacement = number_text(slider);
            else if (inner == "current") replacement = current_text ? current_text : live_current(action);
            else replacement = read_reference(inner);
            // Unresolvable placeholders stay as written so the problem is visible (and numeric parsing fails cleanly).
            out += replacement ? *replacement : text.substr(i, close - i + 1);
            i = close + 1;
        }
        return out;
    }

    auto TrainerEngine::eval_number(const std::string& expression, double slider, const ActionDef* action, double fallback, bool* resolved, std::string* note) -> double
    {
        if (resolved) *resolved = false;
        if (expression.empty()) return fallback;
        const std::string text = expand_text(expression, slider, action);
        if (text.find('{') != std::string::npos)
        {
            if (note) *note = "could not resolve '" + expression + "' (no live object, or no such property)";
            return fallback;
        }
        if (auto value = evaluate_expression(text))
        {
            if (resolved) *resolved = true;
            return *value;
        }
        if (note) *note = "'" + expression + "' is not a number (got '" + text + "')";
        return fallback;
    }

    auto TrainerEngine::compute_view(const CheatDef& cheat) -> SliderView
    {
        SliderView view;
        view.min = cheat.min;
        view.max = cheat.max;
        view.default_value = cheat.default_value;

        // {current} for a slider bound means the current value of the cheat's first property.
        const ActionDef* first = nullptr;
        for (const auto& action : cheat.actions)
        {
            if (!action.property.empty())
            {
                first = &action;
                break;
            }
        }
        std::optional<std::string> current_text;
        if (first)
        {
            if (auto text = live_current(first))
            {
                if (auto number = evaluate_expression(*text))
                {
                    view.has_current = true;
                    view.current = *number;
                    current_text = number_text(*number);
                }
            }
        }

        auto bound = [&](const std::string& expression, double fallback) {
            if (expression.empty()) return fallback;
            const std::string text = expand_text(expression, cheat.default_value, first, current_text);
            if (text.find('{') == std::string::npos)
            {
                if (auto value = evaluate_expression(text)) return *value;
                view.note += "'" + expression + "' is not a number (got '" + text + "'). ";
            }
            else view.note += "could not resolve '" + expression + "'. ";
            return fallback;
        };
        view.min = bound(cheat.min_expr, cheat.min);
        view.max = bound(cheat.max_expr, cheat.max);
        view.default_value = bound(cheat.default_expr, cheat.default_value);
        if (!(view.max > view.min))
        {
            view.note += "max is not above min; widened. ";
            view.max = view.min + 1.0;
        }
        return view;
    }

    auto TrainerEngine::refresh_values_locked() -> void
    {
        m_views.clear();
        for (const auto& trainer : m_trainers)
        {
            for (const auto& cheat : trainer.cheats)
            {
                if (cheat.type == "slider") m_views[key_of(trainer, cheat)] = compute_view(cheat);
            }
        }
        m_views_dirty = false;
    }

    auto TrainerEngine::refresh_values() -> void
    {
        GameThread::Post([this] {
            std::lock_guard lock(m_mutex);
            refresh_values_locked();
        });
    }
} // namespace RC::Toolkit
