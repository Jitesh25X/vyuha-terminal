#include "vyuha/core/slew_check.h"

#include <cstddef>

namespace vyuha {

namespace {

[[nodiscard]] bool is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\r'; }

[[nodiscard]] char lower(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::string_view trim_front(std::string_view s) noexcept {
  while (!s.empty() && is_space(s.front())) {
    s.remove_prefix(1);
  }
  return s;
}

// The text up to the next blank, and leaves the rest in `s`.
[[nodiscard]] std::string_view take_word(std::string_view& s) noexcept {
  s = trim_front(s);
  std::size_t n = 0;
  while (n < s.size() && !is_space(s[n])) {
    ++n;
  }
  const std::string_view word(s.data(), n);
  s.remove_prefix(n);
  return word;
}

// Nothing but a comment may follow the arguments, as chrony treats a stray argument as a config error.
[[nodiscard]] bool nothing_follows(std::string_view rest) noexcept {
  const std::string_view next = take_word(rest);
  return next.empty() || next.front() == '#' || next.front() == '!' || next.front() == ';' ||
         next.front() == '%';
}

// digits[.digits]; false for anything else, so a typo can't pass as a small number.
[[nodiscard]] bool parse_number(std::string_view word, double& out) noexcept {
  double whole = 0.0;
  std::size_t i = 0;
  for (; i < word.size() && word[i] >= '0' && word[i] <= '9'; ++i) {
    whole = (whole * 10.0) + static_cast<double>(word[i] - '0');
  }
  if (i == 0) {
    return false;
  }
  double fraction = 0.0;
  if (i < word.size() && word[i] == '.') {
    double scale = 0.1;
    for (++i; i < word.size() && word[i] >= '0' && word[i] <= '9'; ++i) {
      fraction += scale * static_cast<double>(word[i] - '0');
      scale /= 10.0;
    }
  }
  if (i != word.size()) {
    return false;
  }
  out = whole + fraction;
  return true;
}

// `makestep <threshold> <limit>`: true when the limit is negative, which is "always", and both parse.
[[nodiscard]] bool makestep_is_unlimited(std::string_view args) noexcept {
  double threshold = 0.0;
  if (!parse_number(take_word(args), threshold) || threshold <= 0.0) {
    return false;
  }
  const std::string_view limit = take_word(args);
  if (limit.size() < 2 || limit.front() != '-') {
    return false;
  }
  double magnitude = 0.0;
  // chrony reads the limit as an integer, so -0 is 0, which means never step.
  return parse_number(limit.substr(1), magnitude) && magnitude >= 1.0 && nothing_follows(args);
}

}  // namespace

SlewCheck check_chrony_slew(std::string_view conf) noexcept {
  SlewCheck result{
      .ok = false, .configured = false, .unreadable = false, .ppm = kChronyDefaultSlewPpm, .steps = false};
  double largest = 0.0;
  bool step_lines = false;
  bool all_unlimited = true;
  while (!conf.empty()) {
    const std::size_t end = conf.find('\n');
    std::string_view line =
        trim_front(std::string_view(conf.data(), end == std::string_view::npos ? conf.size() : end));
    conf.remove_prefix(end == std::string_view::npos ? conf.size() : end + 1);
    const std::string_view directive = take_word(line);
    if (iequals(directive, "makestep")) {
      step_lines = true;
      all_unlimited = all_unlimited && makestep_is_unlimited(line);
      continue;
    }
    if (!iequals(directive, "maxslewrate")) {
      continue;  // also skips comments: their first word is never the directive
    }
    double ppm = 0.0;
    if (!parse_number(take_word(line), ppm) || ppm <= 0.0 || !nothing_follows(line)) {
      result.unreadable = true;
      continue;
    }
    result.configured = true;
    largest = ppm > largest ? ppm : largest;
  }
  if (result.configured) {
    result.ppm = largest;
  }
  result.ok = result.configured && !result.unreadable && largest <= kMaxSlewPpm;
  result.steps = step_lines && all_unlimited;
  return result;
}

}  // namespace vyuha
