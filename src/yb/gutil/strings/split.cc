// Copyright 2008 and onwards Google Inc.  All rights reserved.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
// Maintainer: Greg Miller <jgm@google.com>

#include "yb/gutil/strings/split.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <iterator>
using std::back_insert_iterator;
using std::iterator_traits;
#include <limits>
using std::numeric_limits;
using std::string;
using std::vector;
using std::map;
using std::set;
using std::pair;

#include "yb/gutil/integral_types.h"
#include <glog/logging.h>
#include "yb/gutil/logging-inl.h"
#include "yb/gutil/macros.h"
#include "yb/gutil/strtoint.h"
#include "yb/gutil/strings/ascii_ctype.h"
#include "yb/gutil/strings/util.h"
#include "yb/gutil/hash/hash.h"

// Implementations for some of the Split2 API. Much of the Split2 API is
// templated so it exists in header files, either strings/split.h or
// strings/split_iternal.h.
namespace strings {
namespace delimiter {

namespace {

// This GenericFind() template function encapsulates the finding algorithm
// shared between the Literal and AnyOf delimiters. The FindPolicy template
// parameter allows each delimiter to customize the actual find function to use
// and the length of the found delimiter. For example, the Literal delimiter
// will ultimately use GStringPiece::find(), and the AnyOf delimiter will use
// GStringPiece::find_first_of().
template <typename FindPolicy>
GStringPiece GenericFind(
    GStringPiece text,
    GStringPiece delimiter,
    FindPolicy find_policy) {
  if (delimiter.empty() && text.length() > 0) {
    // Special case for empty string delimiters: always return a zero-length
    // GStringPiece referring to the item at position 1.
    return GStringPiece(text.begin() + 1, 0);
  }
  auto found_pos = GStringPiece::npos;
  GStringPiece found(text.end(), 0);  // By default, not found
  found_pos = find_policy.Find(text, delimiter);
  if (found_pos != GStringPiece::npos) {
    found.set(text.data() + found_pos, find_policy.Length(delimiter));
  }
  return found;
}

// Finds using GStringPiece::find(), therefore the length of the found delimiter
// is delimiter.length().
struct LiteralPolicy {
  size_t Find(GStringPiece text, GStringPiece delimiter) {
    return text.find(delimiter);
  }
  size_t Length(GStringPiece delimiter) {
    return delimiter.length();
  }
};

// Finds using GStringPiece::find_first_of(), therefore the length of the found
// delimiter is 1.
struct AnyOfPolicy {
  size_t Find(GStringPiece text, GStringPiece delimiter) {
    return text.find_first_of(delimiter);
  }
  size_t Length(GStringPiece delimiter) {
    return 1;
  }
};

}  // namespace

//
// Literal
//

Literal::Literal(GStringPiece sp) : delimiter_(sp.ToString()) {
}

GStringPiece Literal::Find(GStringPiece text) const {
  return GenericFind(text, delimiter_, LiteralPolicy());
}

//
// AnyOf
//

AnyOf::AnyOf(GStringPiece sp) : delimiters_(sp.ToString()) {
}

GStringPiece AnyOf::Find(GStringPiece text) const {
  return GenericFind(text, delimiters_, AnyOfPolicy());
}

}  // namespace delimiter
}  // namespace strings

//
// ==================== LEGACY SPLIT FUNCTIONS ====================
//

using ::strings::SkipEmpty;
using ::strings::delimiter::AnyOf;
using ::strings::delimiter::Limit;

namespace {

// Appends the results of a split to the specified container. This function has
// the following overloads:
// - vector<string>           - for better performance
// - map<string, string>      - to change append semantics
// - hash_map<string, string> - to change append semantics
template <typename Container, typename Splitter>
void AppendToImpl(Container* container, Splitter splitter) {
  Container c = splitter;  // Calls implicit conversion operator.
  std::copy(c.begin(), c.end(), std::inserter(*container, container->end()));
}

// Overload of AppendToImpl() that is optimized for appending to vector<string>.
// This version eliminates a couple string copies by using a vector<GStringPiece>
// as the intermediate container.
template <typename Splitter>
void AppendToImpl(vector<string>* container, Splitter splitter) {
  vector<GStringPiece> vsp = splitter;  // Calls implicit conversion operator.
  size_t container_size = container->size();
  container->resize(container_size + vsp.size());
  for (const auto& sp : vsp) {
    sp.CopyToString(&(*container)[container_size++]);
  }
}

// Here we define two AppendToImpl() overloads for map<> and hash_map<>. Both of
// these overloads call through to this AppendToMap() function. This is needed
// because inserting a duplicate key into a map does NOT overwrite the previous
// value, which was not the behavior of the split1 Split*() functions. Consider
// this example:
//
//   map<string, string> m;
//   m.insert(std::make_pair("a", "1"));
//   m.insert(std::make_pair("a", "2"));  // <-- doesn't actually insert.
//   ASSERT_EQ(m["a"], "1");  // <-- "a" has value "1" not "2".
//
// Due to this behavior of map::insert, we can't rely on a normal std::inserter
// for a maps. Instead, maps and hash_maps need to be special cased to implement
// the desired append semantic of inserting an existing value overwrites the
// previous value.
//
// This same issue is true with sets as well. However, since sets don't have a
// separate key and value, failing to overwrite an existing value in a set is
// fine because the value already exists in the set.
//
template <typename Map, typename Splitter>
void AppendToMap(Map* m, Splitter splitter) {
  Map tmp = splitter;  // Calls implicit conversion operator.
  for (typename Map::const_iterator it = tmp.begin(); it != tmp.end(); ++it) {
    (*m)[it->first] = it->second;
  }
}

template <typename Splitter>
void AppendToImpl(map<string, string>* map_container, Splitter splitter) {
  AppendToMap(map_container, splitter);
}

// Appends the results of a call to strings::Split() to the specified container.
// This function is used with the new strings::Split() API to implement the
// append semantics of the legacy Split*() functions.
//
// The "Splitter" template parameter is intended to be a
// ::strings::internal::Splitter<>, which is the return value of a call to
// strings::Split(). Sample usage:
//
//   vector<string> v;
//   ... add stuff to "v" ...
//   AppendTo(&v, strings::Split("a,b,c", ","));
//
template <typename Container, typename Splitter>
void AppendTo(Container* container, Splitter splitter) {
  if (container->empty()) {
    // "Appending" to an empty container is by far the common case. For this we
    // assign directly to the output container, which is more efficient than
    // explicitly appending.
    *container = splitter;  // Calls implicit conversion operator.
  } else {
    AppendToImpl(container, splitter);
  }
}

}  // anonymous namespace

// Constants for ClipString()
static const int kMaxOverCut = 12;
// The ellipsis to add to strings that are too long
static const char kCutStr[] = "...";
static const size_t kCutStrSize = sizeof(kCutStr) - 1;

// ----------------------------------------------------------------------
// Return the place to clip the string at, or -1
// if the string doesn't need to be clipped.
// ----------------------------------------------------------------------
static size_t ClipStringHelper(const char* str, size_t max_len, bool use_ellipsis) {
  if (strlen(str) <= max_len)
    return std::numeric_limits<size_t>::max();

  auto max_substr_len = max_len;

  if (use_ellipsis && max_len > kCutStrSize) {
    max_substr_len -= kCutStrSize;
  }

  const char* cut_by =
      (max_substr_len < kMaxOverCut ? str : str + max_len - kMaxOverCut);
  const char* cut_at = str + max_substr_len;
  while (!ascii_isspace(*cut_at) && cut_at > cut_by)
    cut_at--;

  if (cut_at == cut_by) {
    // No space was found
    return max_substr_len;
  } else {
    return cut_at-str;
  }
}

// ----------------------------------------------------------------------
// ClipString
//    Clip a string to a max length. We try to clip on a word boundary
//    if this is possible. If the string is clipped, we append an
//    ellipsis.
// ----------------------------------------------------------------------

void ClipString(char* str, size_t max_len) {
  auto cut_at = ClipStringHelper(str, max_len, true);
  if (cut_at != std::numeric_limits<size_t>::max()) {
    if (max_len > kCutStrSize) {
      strcpy(str+cut_at, kCutStr); // NOLINT
    } else {
      strcpy(str+cut_at, ""); // NOLINT
    }
  }
}

// ----------------------------------------------------------------------
// ClipString
//    Version of ClipString() that uses string instead of char*.
// ----------------------------------------------------------------------
void ClipString(string* full_str, size_t max_len) {
  auto cut_at = ClipStringHelper(full_str->c_str(), max_len, true);
  if (cut_at != std::numeric_limits<size_t>::max()) {
    full_str->erase(cut_at);
    if (max_len > kCutStrSize) {
      full_str->append(kCutStr);
    }
  }
}

// ----------------------------------------------------------------------
// SplitStringToIteratorAllowEmpty()
//    Split a string using a character delimiter. Append the components
//    to 'result'.  If there are consecutive delimiters, this function
//    will return corresponding empty strings. The string is split into
//    at most the specified number of pieces greedily. This means that the
//    last piece may possibly be split further. To split into as many pieces
//    as possible, specify 0 as the number of pieces.
//
//    If "full" is the empty string, yields an empty string as the only value.
//
//    If "pieces" is negative for some reason, it returns the whole string
// ----------------------------------------------------------------------
template <typename StringType, typename ITR>
static inline
void SplitStringToIteratorAllowEmpty(const StringType& full,
                                     const char* delim,
                                     size_t pieces,
                                     ITR& result) { // NOLINT
  string::size_type begin_index, end_index;
  begin_index = 0;

  for (size_t i = 0; (i < pieces-1) || (pieces == 0); i++) {
    end_index = full.find_first_of(delim, begin_index);
    if (end_index == string::npos) {
      *result++ = full.substr(begin_index);
      return;
    }
    *result++ = full.substr(begin_index, (end_index - begin_index));
    begin_index = end_index + 1;
  }
  *result++ = full.substr(begin_index);
}

void SplitStringIntoNPiecesAllowEmpty(const string& full,
                                      const char* delim,
                                      size_t pieces,
                                      vector<string>* result) {
  if (pieces == 0) {
    // No limit when pieces is 0.
    AppendTo(result, strings::Split(full, AnyOf(delim)));
  } else {
    // The input argument "pieces" specifies the max size that *result should
    // be. However, the argument to the Limit() delimiter is the max number of
    // delimiters, which should be one less than "pieces". Example: "a,b,c" has
    // 3 pieces and two comma delimiters.
    auto limit = std::max<size_t>(pieces - 1, 0);
    AppendTo(result, strings::Split(full, Limit(AnyOf(delim), limit)));
  }
}

// ----------------------------------------------------------------------
// SplitStringAllowEmpty
//    Split a string using a character delimiter. Append the components
//    to 'result'.  If there are consecutive delimiters, this function
//    will return corresponding empty strings.
// ----------------------------------------------------------------------
void SplitStringAllowEmpty(const string& full, const char* delim,
                           vector<string>* result) {
  AppendTo(result, strings::Split(full, AnyOf(delim)));
}

// If we know how much to allocate for a vector of strings, we can
// allocate the vector<string> only once and directly to the right size.
// This saves in between 33-66 % of memory space needed for the result,
// and runs faster in the microbenchmarks.
//
// The reserve is only implemented for the single character delim.
//
// The implementation for counting is cut-and-pasted from
// SplitStringToIteratorUsing. I could have written my own counting iterator,
// and use the existing template function, but probably this is more clear
// and more sure to get optimized to reasonable code.
static size_t CalculateReserveForVector(const string& full, const char* delim) {
  size_t count = 0;
  if (delim[0] != '\0' && delim[1] == '\0') {
    // Optimize the common case where delim is a single character.
    char c = delim[0];
    const char* p = full.data();
    const char* end = p + full.size();
    while (p != end) {
      if (*p == c) {  // This could be optimized with hasless(v,1) trick.
        ++p;
      } else {
        while (++p != end && *p != c) {
          // Skip to the next occurence of the delimiter.
        }
        ++count;
      }
    }
  }
  return count;
}

// ----------------------------------------------------------------------
// SplitStringUsing()
// SplitStringToHashsetUsing()
// SplitStringToSetUsing()
// SplitStringToMapUsing()
// SplitStringToHashmapUsing()
//    Split a string using a character delimiter. Append the components
//    to 'result'.
//
// Note: For multi-character delimiters, this routine will split on *ANY* of
// the characters in the string, not the entire string as a single delimiter.
// ----------------------------------------------------------------------
template <typename StringType, typename ITR>
static inline
void SplitStringToIteratorUsing(const StringType& full,
                                const char* delim,
                                ITR& result) { // NOLINT
  // Optimize the common case where delim is a single character.
  if (delim[0] != '\0' && delim[1] == '\0') {
    char c = delim[0];
    const char* p = full.data();
    const char* end = p + full.size();
    while (p != end) {
      if (*p == c) {
        ++p;
      } else {
        const char* start = p;
        while (++p != end && *p != c) {
          // Skip to the next occurence of the delimiter.
        }
        *result++ = StringType(start, p - start);
      }
    }
    return;
  }

  string::size_type begin_index, end_index;
  begin_index = full.find_first_not_of(delim);
  while (begin_index != string::npos) {
    end_index = full.find_first_of(delim, begin_index);
    if (end_index == string::npos) {
      *result++ = full.substr(begin_index);
      return;
    }
    *result++ = full.substr(begin_index, (end_index - begin_index));
    begin_index = full.find_first_not_of(delim, end_index);
  }
}

void SplitStringUsing(const string& full,
                      const char* delim,
                      vector<string>* result) {
  result->reserve(result->size() + CalculateReserveForVector(full, delim));
  std::back_insert_iterator< vector<string> > it(*result);
  SplitStringToIteratorUsing(full, delim, it);
}

void SplitStringToSetUsing(const string& full, const char* delim,
                           set<string>* result) {
  AppendTo(result, strings::Split(full, AnyOf(delim), strings::SkipEmpty()));
}

void SplitStringToMapUsing(const string& full, const char* delim,
                           map<string, string>* result) {
  AppendTo(result, strings::Split(full, AnyOf(delim), strings::SkipEmpty()));
}

// ----------------------------------------------------------------------
// SplitGStringPieceToVector()
//    Split a GStringPiece into sub-GStringPieces based on delim
//    and appends the pieces to 'vec'.
//    If omit empty strings is true, empty strings are omitted
//    from the resulting vector.
// ----------------------------------------------------------------------
void SplitGStringPieceToVector(const GStringPiece& full,
                              const char* delim,
                              vector<GStringPiece>* vec,
                              bool omit_empty_strings) {
  if (omit_empty_strings) {
    AppendTo(vec, strings::Split(full, AnyOf(delim), SkipEmpty()));
  } else {
    AppendTo(vec, strings::Split(full, AnyOf(delim)));
  }
}

// ----------------------------------------------------------------------
// SplitUsing()
//    Split a string using a string of delimiters, returning vector
//    of strings. The original string is modified to insert nulls.
// ----------------------------------------------------------------------

vector<char*>* SplitUsing(char* full, const char* delim) {
  auto vec = new vector<char*>;
  SplitToVector(full, delim, vec, true);        // Omit empty strings
  return vec;
}

void SplitToVector(char* full, const char* delim, vector<char*>* vec,
                   bool omit_empty_strings) {
  char* next  = full;
  while ((next = gstrsep(&full, delim)) != nullptr) {
    if (omit_empty_strings && next[0] == '\0') continue;
    vec->push_back(next);
  }
  // Add last element (or full string if no delimeter found):
  if (full != nullptr) {
    vec->push_back(full);
  }
}

void SplitToVector(char* full, const char* delim, vector<const char*>* vec,
                   bool omit_empty_strings) {
  char* next  = full;
  while ((next = gstrsep(&full, delim)) != nullptr) {
    if (omit_empty_strings && next[0] == '\0') continue;
    vec->push_back(next);
  }
  // Add last element (or full string if no delimeter found):
  if (full != nullptr) {
    vec->push_back(full);
  }
}

// ----------------------------------------------------------------------
// SplitOneStringToken()
//   Mainly a stringified wrapper around strpbrk()
// ----------------------------------------------------------------------
string SplitOneStringToken(const char** source, const char* delim) {
  assert(source);
  assert(delim);
  if (!*source) {
    return string();
  }
  const char * begin = *source;
  // Optimize the common case where delim is a single character.
  if (delim[0] != '\0' && delim[1] == '\0') {
    *source = strchr(*source, delim[0]);
  } else {
    *source = strpbrk(*source, delim);
  }
  if (*source) {
    return string(begin, (*source)++);
  } else {
    return string(begin);
  }
}

// ----------------------------------------------------------------------
// SplitStringWithEscaping()
// SplitStringWithEscapingAllowEmpty()
// SplitStringWithEscapingToSet()
// SplitStringWithWithEscapingToHashset()
//   Split the string using the specified delimiters, taking escaping into
//   account. '\' is not allowed as a delimiter.
// ----------------------------------------------------------------------
template <typename ITR>
static inline
void SplitStringWithEscapingToIterator(const string& src,
                                       const strings::CharSet& delimiters,
                                       const bool allow_empty,
                                       ITR* result) {
  CHECK(!delimiters.Test('\\')) << "\\ is not allowed as a delimiter.";
  CHECK(result);
  string part;

  for (uint32 i = 0; i < src.size(); ++i) {
    char current_char = src[i];
    if (delimiters.Test(current_char)) {
      // Push substrings when we encounter delimiters.
      if (allow_empty || !part.empty()) {
        *(*result)++ = part;
        part.clear();
      }
    } else if (current_char == '\\' && ++i < src.size()) {
      // If we see a backslash, the next delimiter or backslash is literal.
      current_char = src[i];
      if (current_char != '\\' && !delimiters.Test(current_char)) {
        // Don't honour unknown escape sequences: emit \f for \f.
        part.push_back('\\');
      }
      part.push_back(current_char);
    } else {
      // Otherwise, we have a normal character or trailing backslash.
      part.push_back(current_char);
    }
  }

  // Push the trailing part.
  if (allow_empty || !part.empty()) {
    *(*result)++ = part;
  }
}

void SplitStringWithEscaping(const string &full,
                             const strings::CharSet& delimiters,
                             vector<string> *result) {
  std::back_insert_iterator< vector<string> > it(*result);
  SplitStringWithEscapingToIterator(full, delimiters, false, &it);
}

void SplitStringWithEscapingAllowEmpty(const string &full,
                                       const strings::CharSet& delimiters,
                                       vector<string> *result) {
  std::back_insert_iterator< vector<string> > it(*result);
  SplitStringWithEscapingToIterator(full, delimiters, true, &it);
}

void SplitStringWithEscapingToSet(const string &full,
                                  const strings::CharSet& delimiters,
                                  set<string> *result) {
  std::insert_iterator< set<string> > it(*result, result->end());
  SplitStringWithEscapingToIterator(full, delimiters, false, &it);
}

// ----------------------------------------------------------------------
// SplitOneIntToken()
// SplitOneInt32Token()
// SplitOneUint32Token()
// SplitOneInt64Token()
// SplitOneUint64Token()
// SplitOneDoubleToken()
// SplitOneFloatToken()
// SplitOneDecimalIntToken()
// SplitOneDecimalInt32Token()
// SplitOneDecimalUint32Token()
// SplitOneDecimalInt64Token()
// SplitOneDecimalUint64Token()
// SplitOneHexUint32Token()
// SplitOneHexUint64Token()
//   Mainly a stringified wrapper around strtol/strtoul/strtod
// ----------------------------------------------------------------------
// Curried functions for the macro below
static inline int32_t strto32_0(const char* source, char** end) {
  return strto32(source, end, 0); }
static inline uint32_t strtou32_0(const char* source, char** end) {
  return strtou32(source, end, 0); }
static inline int64 strto64_0(const char* source, char** end) {
  return strto64(source, end, 0); }
static inline uint64 strtou64_0(const char* source, char** end) {
  return strtou64(source, end, 0); }
static inline int32_t strto32_10(const char* source, char** end) {
  return strto32(source, end, 10); }
static inline uint32_t strtou32_10(const char* source, char** end) {
  return strtou32(source, end, 10); }
static inline int64 strto64_10(const char* source, char** end) {
  return strto64(source, end, 10); }
static inline uint64 strtou64_10(const char* source, char** end) {
  return strtou64(source, end, 10); }
static inline uint32 strtou32_16(const char* source, char** end) {
  return strtou32(source, end, 16); }
static inline uint64 strtou64_16(const char* source, char** end) {
  return strtou64(source, end, 16); }

#define DEFINE_SPLIT_ONE_NUMBER_TOKEN(name, type, function) \
bool SplitOne##name##Token(const char ** source, const char * delim, \
                           type * value) {                      \
  assert(source);                                               \
  assert(delim);                                                \
  assert(value);                                                \
  if (!*source) {                                               \
    return false;                                               \
  }                                                             \
  /* Parse int */                                               \
  char * end;                                                   \
  *value = function(*source, &end);                             \
  if (end == *source)                                           \
    return false; /* number not present at start of string */   \
  if (end[0] && !strchr(delim, end[0])) {                       \
    return false; /* Garbage characters after int */            \
  }                                                             \
  /* Advance past token */                                      \
  if (*end != '\0')                                             \
    *source = const_cast<const char *>(end+1);                  \
  else                                                          \
    *source = NULL;                                             \
  return true;                                                  \
}

DEFINE_SPLIT_ONE_NUMBER_TOKEN(Int, int, strto32_0)
DEFINE_SPLIT_ONE_NUMBER_TOKEN(Int32, int32, strto32_0)
DEFINE_SPLIT_ONE_NUMBER_TOKEN(Uint32, uint32, strtou32_0)
DEFINE_SPLIT_ONE_NUMBER_TOKEN(Int64, int64, strto64_0)
DEFINE_SPLIT_ONE_NUMBER_TOKEN(Uint64, uint64, strtou64_0)
DEFINE_SPLIT_ONE_NUMBER_TOKEN(Double, double, strtod)
#ifdef _MSC_VER  // has no strtof()
// Note: does an implicit cast to float.
DEFINE_SPLIT_ONE_NUMBER_TOKEN(Float, float, strtod)
#else
DEFINE_SPLIT_ONE_NUMBER_TOKEN(Float, float, strtof)
#endif
DEFINE_SPLIT_ONE_NUMBER_TOKEN(DecimalInt, int, strto32_10)
DEFINE_SPLIT_ONE_NUMBER_TOKEN(DecimalInt32, int32, strto32_10)
DEFINE_SPLIT_ONE_NUMBER_TOKEN(DecimalUint32, uint32, strtou32_10)
DEFINE_SPLIT_ONE_NUMBER_TOKEN(DecimalInt64, int64, strto64_10)
DEFINE_SPLIT_ONE_NUMBER_TOKEN(DecimalUint64, uint64, strtou64_10)
DEFINE_SPLIT_ONE_NUMBER_TOKEN(HexUint32, uint32, strtou32_16)
DEFINE_SPLIT_ONE_NUMBER_TOKEN(HexUint64, uint64, strtou64_16)


// ----------------------------------------------------------------------
// SplitRange()
//    Splits a string of the form "<from>-<to>".  Either or both can be
//    missing.  A raw number (<to>) is interpreted as "<to>-".  Modifies
//    parameters insofar as they're specified by the string.  RETURNS
//    true iff the input is a well-formed range.  If it RETURNS false,
//    from and to remain unchanged.  The range in rangestr should be
//    terminated either by "\0" or by whitespace.
// ----------------------------------------------------------------------

#define EOS(ch)  ( (ch) == '\0' || ascii_isspace(ch) )
bool SplitRange(const char* rangestr, int* from, int* to) {
  // We need to do the const-cast because strol takes a char**, not const char**
  char* val = const_cast<char*>(rangestr);
  if (val == nullptr || EOS(*val))  return true;  // we'll say nothingness is ok

  if ( val[0] == '-' && EOS(val[1]) )    // CASE 1: -
    return true;                         // nothing changes

  if ( val[0] == '-' ) {                 // CASE 2: -<i2>
    const int int2 = strto32(val+1, &val, 10);
    if ( !EOS(*val) )  return false;     // not a valid integer
    *to = int2;                          // only "to" changes
    return true;

  } else {
    const int int1 = strto32(val, &val, 10);
    if ( EOS(*val) || (*val == '-' && EOS(*(val+1))) ) {
      *from = int1;                      // CASE 3: <i1>, same as <i1>-
      return true;                       // only "from" changes
    } else if (*val != '-') {            // not a valid range
      return false;
    }
    const int int2 = strto32(val+1, &val, 10);
    if ( !EOS(*val) )  return false;     // not a valid integer
    *from = int1;                        // CASE 4: <i1>-<i2>
    *to = int2;
    return true;
  }
}

void SplitCSVLineWithDelimiter(char* line, char delimiter,
                               vector<char*>* cols) {
  char* end_of_line = line + strlen(line);
  char* end;
  char* start;

  for (; line < end_of_line; line++) {
    // Skip leading whitespace, unless said whitespace is the delimiter.
    while (ascii_isspace(*line) && *line != delimiter)
      ++line;

    if (*line == '"' && delimiter == ',') {     // Quoted value...
      start = ++line;
      end = start;
      for (; *line; line++) {
        if (*line == '"') {
          line++;
          if (*line != '"')  // [""] is an escaped ["]
            break;           // but just ["] is end of value
        }
        *end++ = *line;
      }
      // All characters after the closing quote and before the comma
      // are ignored.
      line = strchr(line, delimiter);
      if (!line) line = end_of_line;
    } else {
      start = line;
      line = strchr(line, delimiter);
      if (!line) line = end_of_line;
      // Skip all trailing whitespace, unless said whitespace is the delimiter.
      for (end = line; end > start; --end) {
        if (!ascii_isspace(end[-1]) || end[-1] == delimiter)
          break;
      }
    }
    const bool need_another_column =
      (*line == delimiter) && (line == end_of_line - 1);
    *end = '\0';
    cols->push_back(start);
    // If line was something like [paul,] (comma is the last character
    // and is not proceeded by whitespace or quote) then we are about
    // to eliminate the last column (which is empty). This would be
    // incorrect.
    if (need_another_column)
      cols->push_back(end);

    assert(*line == '\0' || *line == delimiter);
  }
}

void SplitCSVLine(char* line, vector<char*>* cols) {
  SplitCSVLineWithDelimiter(line, ',', cols);
}

void SplitCSVLineWithDelimiterForStrings(const string &line,
                                         char delimiter,
                                         vector<string> *cols) {
  // Unfortunately, the interface requires char* instead of const char*
  // which requires copying the string.
  char *cline = strndup_with_new(line.c_str(), line.size());
  vector<char *> v;
  SplitCSVLineWithDelimiter(cline, delimiter, &v);
  for (vector<char*>::const_iterator ci = v.begin(); ci != v.end(); ++ci) {
    cols->push_back(*ci);
  }
  delete[] cline;
}

// ----------------------------------------------------------------------
namespace {

// Helper class used by SplitStructuredLineInternal.
class ClosingSymbolLookup {
 public:
  explicit ClosingSymbolLookup(const char* symbol_pairs)
      : closing_(),
        valid_closing_() {
    // Initialize the opening/closing arrays.
    for (const char* symbol = symbol_pairs; *symbol != 0; ++symbol) {
      unsigned char opening = *symbol;
      ++symbol;
      // If the string ends before the closing character has been found,
      // use the opening character as the closing character.
      unsigned char closing = *symbol != 0 ? *symbol : opening;
      closing_[opening] = closing;
      valid_closing_[closing] = true;
      if (*symbol == 0) break;
    }
  }

  // Returns the closing character corresponding to an opening one,
  // or 0 if the argument is not an opening character.
  char GetClosingChar(char opening) const {
    return closing_[static_cast<unsigned char>(opening)];
  }

  // Returns true if the argument is a closing character.
  bool IsClosing(char c) const {
    return valid_closing_[static_cast<unsigned char>(c)];
  }

 private:
  // Maps an opening character to its closing. If the entry contains 0,
  // the character is not in the opening set.
  char closing_[256];
  // Valid closing characters.
  bool valid_closing_[256];

  DISALLOW_COPY_AND_ASSIGN(ClosingSymbolLookup);
};

char* SplitStructuredLineInternal(char* line,
                                  char delimiter,
                                  const char* symbol_pairs,
                                  vector<char*>* cols,
                                  bool with_escapes) {
  ClosingSymbolLookup lookup(symbol_pairs);

  // Stack of symbols expected to close the current opened expressions.
  vector<char> expected_to_close;
  bool in_escape = false;

  CHECK(cols);
  cols->push_back(line);
  char* current;
  for (current = line; *current; ++current) {
    char c = *current;
    if (in_escape) {
      in_escape = false;
    } else if (with_escapes && c == '\\') {
      // We are escaping the next character. Note the escape still appears
      // in the output.
      in_escape = true;
    } else if (expected_to_close.empty() && c == delimiter) {
      // We don't have any open expression, this is a valid separator.
      *current = 0;
      cols->push_back(current + 1);
    } else if (!expected_to_close.empty() && c == expected_to_close.back()) {
      // Can we close the currently open expression?
      expected_to_close.pop_back();
    } else if (lookup.GetClosingChar(c)) {
      // If this is an opening symbol, we open a new expression and push
      // the expected closing symbol on the stack.
      expected_to_close.push_back(lookup.GetClosingChar(c));
    } else if (lookup.IsClosing(c)) {
      // Error: mismatched closing symbol.
      return current;
    }
  }
  if (!expected_to_close.empty()) {
    return current;  // Missing closing symbol(s)
  }
  return nullptr;  // Success
}

bool SplitStructuredLineInternal(GStringPiece line,
                                 char delimiter,
                                 const char* symbol_pairs,
                                 vector<GStringPiece>* cols,
                                 bool with_escapes) {
  ClosingSymbolLookup lookup(symbol_pairs);

  // Stack of symbols expected to close the current opened expressions.
  vector<char> expected_to_close;
  bool in_escape = false;

  CHECK_NOTNULL(cols);
  cols->push_back(line);
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (in_escape) {
      in_escape = false;
    } else if (with_escapes && c == '\\') {
      // We are escaping the next character. Note the escape still appears
      // in the output.
      in_escape = true;
    } else if (expected_to_close.empty() && c == delimiter) {
      // We don't have any open expression, this is a valid separator.
      cols->back().remove_suffix(line.size() - i);
      cols->push_back(GStringPiece(line, i + 1));
    } else if (!expected_to_close.empty() && c == expected_to_close.back()) {
      // Can we close the currently open expression?
      expected_to_close.pop_back();
    } else if (lookup.GetClosingChar(c)) {
      // If this is an opening symbol, we open a new expression and push
      // the expected closing symbol on the stack.
      expected_to_close.push_back(lookup.GetClosingChar(c));
    } else if (lookup.IsClosing(c)) {
      // Error: mismatched closing symbol.
      return false;
    }
  }
  if (!expected_to_close.empty()) {
    return false;  // Missing closing symbol(s)
  }
  return true;  // Success
}

}  // anonymous namespace

char* SplitStructuredLine(char* line,
                          char delimiter,
                          const char *symbol_pairs,
                          vector<char*>* cols) {
  return SplitStructuredLineInternal(line, delimiter, symbol_pairs, cols,
                                     false);
}

bool SplitStructuredLine(GStringPiece line,
                         char delimiter,
                         const char* symbol_pairs,
                         vector<GStringPiece>* cols) {
  return SplitStructuredLineInternal(line, delimiter, symbol_pairs, cols,
                                     false);
}

char* SplitStructuredLineWithEscapes(char* line,
                                     char delimiter,
                                     const char *symbol_pairs,
                                     vector<char*>* cols) {
  return SplitStructuredLineInternal(line, delimiter, symbol_pairs, cols,
                                     true);
}

bool SplitStructuredLineWithEscapes(GStringPiece line,
                                     char delimiter,
                                     const char* symbol_pairs,
                                     vector<GStringPiece>* cols) {
  return SplitStructuredLineInternal(line, delimiter, symbol_pairs, cols,
                                     true);
}


// ----------------------------------------------------------------------
// SplitStringIntoKeyValues()
// ----------------------------------------------------------------------
bool SplitStringIntoKeyValues(const string& line,
                              const string& key_value_delimiters,
                              const string& value_value_delimiters,
                              string *key, vector<string> *values) {
  key->clear();
  values->clear();

  // find the key string
  size_t end_key_pos = line.find_first_of(key_value_delimiters);
  if (end_key_pos == string::npos) {
    VLOG(1) << "cannot parse key from line: " << line;
    return false;    // no key
  }
  key->assign(line, 0, end_key_pos);

  // find the values string
  string remains(line, end_key_pos, line.size() - end_key_pos);
  size_t begin_values_pos = remains.find_first_not_of(key_value_delimiters);
  if (begin_values_pos == string::npos) {
    VLOG(1) << "cannot parse value from line: " << line;
    return false;   // no value
  }
  string values_string(remains,
                       begin_values_pos,
                       remains.size() - begin_values_pos);

  // construct the values vector
  if (value_value_delimiters.empty()) {  // one value
    values->push_back(values_string);
  } else {                               // multiple values
    SplitStringUsing(values_string, value_value_delimiters.c_str(), values);
    if (values->size() < 1) {
      VLOG(1) << "cannot parse value from line: " << line;
      return false;  // no value
    }
  }
  return true;
}

bool SplitStringIntoKeyValuePairs(const string& line,
                                  const string& key_value_delimiters,
                                  const string& key_value_pair_delimiters,
                                  vector<pair<string, string> >* kv_pairs) {
  kv_pairs->clear();

  vector<string> pairs;
  SplitStringUsing(line, key_value_pair_delimiters.c_str(), &pairs);

  bool success = true;
  for (const auto& pair : pairs) {
    string key;
    vector<string> value;
    if (!SplitStringIntoKeyValues(pair,
                                  key_value_delimiters,
                                  "", &key, &value)) {
      // Don't return here, to allow for keys without associated
      // values; just record that our split failed.
      success = false;
    }
    // we expect atmost one value because we passed in an empty vsep to
    // SplitStringIntoKeyValues
    DCHECK_LE(value.size(), 1);
    kv_pairs->push_back(make_pair(key, value.empty()? "" : value[0]));
  }
  return success;
}

// ----------------------------------------------------------------------
// SplitLeadingDec32Values()
// SplitLeadingDec64Values()
//    A simple parser for space-separated decimal int32/int64 values.
//    Appends parsed integers to the end of the result vector, stopping
//    at the first unparsable spot.  Skips past leading and repeated
//    whitespace (does not consume trailing whitespace), and returns
//    a pointer beyond the last character parsed.
// --------------------------------------------------------------------
const char* SplitLeadingDec32Values(const char *str, vector<int32> *result) {
  for (;;) {
    char *end = nullptr;
    int64_t value = strtol(str, &end, 10);
    if (end == str)
      break;
    // Limit long values to int32 min/max.  Needed for lp64.
    if (value > numeric_limits<int32>::max()) {
      value = numeric_limits<int32>::max();
    } else if (value < numeric_limits<int32>::min()) {
      value = numeric_limits<int32>::min();
    }
    result->push_back(narrow_cast<int32>(value));
    str = end;
    if (!ascii_isspace(*end))
      break;
  }
  return str;
}

const char* SplitLeadingDec64Values(const char *str, vector<int64> *result) {
  for (;;) {
    char *end = nullptr;
    const int64 value = strtoll(str, &end, 10);
    if (end == str)
      break;
    result->push_back(value);
    str = end;
    if (!ascii_isspace(*end))
      break;
  }
  return str;
}

void SplitStringToLines(const char* full,
                        size_t max_len,
                        size_t num_lines,
                        vector<string>* result) {
  if (max_len <= 0) {
    return;
  }
  size_t pos = 0;
  for (size_t i = 0; (i < num_lines || num_lines <= 0); i++) {
    auto cut_at = ClipStringHelper(full+pos, max_len, (i == num_lines - 1));
    if (cut_at == std::numeric_limits<size_t>::max()) {
      result->push_back(string(full+pos));
      return;
    }
    result->push_back(string(full+pos, cut_at));
    if (i == num_lines - 1 && max_len > kCutStrSize) {
      result->at(i).append(kCutStr);
    }
    pos += cut_at;
  }
}
