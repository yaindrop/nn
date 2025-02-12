#pragma once

/*
 * Copyright (c) 2015 Dropbox, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cstdlib>
#include <functional>
#include <memory>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace util {

// Helper to get the type pointed to by a raw or smart pointer. This can be
// explicitly specialized if need be to provide compatibility with user-defined
// smart pointers.
namespace nn_detail {

template <typename T>
struct element_type {
  using type = typename T::element_type;
};

template <typename Pointee>
struct element_type<Pointee*> {
  using type = Pointee;
};

template <typename T>
void check(const T& v, const std::source_location& loc) {
  if (!v) {
    std::stringstream ss;
    ss << "nn check failed at " << loc.line() << ":" << loc.column() << " of "
       << loc.file_name();
    throw std::runtime_error(ss.str());
  }
}

}  // namespace nn_detail

template <typename PtrType>
class nn;

// Trait to check whether a given type is a non-nullable pointer
template <typename T>
concept IsNN = requires(T t) {
  {
    std::is_same_v<decltype(t), nn<typename T::element_type>>
  } -> std::same_as<bool>;
};

/* nn<PtrType>
 *
 * Wrapper around a pointer that is guaranteed to not be null. This works with
 * raw pointers as well as any smart pointer: nn<int *>,
 * nn<shared_ptr<DbxTable>>, nn<unique_ptr<Foo>>, etc. An nn<PtrType> can be
 * used just like a PtrType.
 *
 * An nn<PtrType> can be constructed from another nn<PtrType>, if the underlying
 * type would allow such construction. For example, nn<shared_ptr<PtrType>> can
 * be copied and moved, but nn<unique_ptr<PtrType>> can only be moved; an
 * nn<unique_ptr<PtrType>> can be explicitly (but not implicitly) created from
 * an nn<PtrType*>; implicit upcasts are allowed; and so on.
 *
 * Similarly, non-nullable pointers can be compared with regular or other
 * non-nullable pointers, using the same rules as the underlying pointer types.
 *
 * This module also provides helpers for creating an nn<PtrType> from operations
 * that would always return a non-null pointer: nn_make_unique, nn_make_shared,
 * nn_shared_from_this, and nn_addr (a replacement for operator&).
 *
 * We abbreviate nn<unique_ptr> as nn_unique_ptr - it's a little more readable.
 * Likewise, nn<shared_ptr> can be written as nn_shared_ptr.
 */
template <typename PtrType>
class nn {
 public:
  static_assert(!IsNN<PtrType>, "nn<nn<T>> is disallowed");

  using element_type = typename nn_detail::element_type<PtrType>::type;

  // Pass through calls to operator* and operator-> transparently
  element_type& operator*() const { return *ptr; }
  element_type* operator->() const { return &*ptr; }

  // Expose the underlying PtrType
  operator const PtrType&() const& { return ptr; }
  operator PtrType&&() && { return std::move(ptr); }

  // Trying to use the assignment operator to assign a nn<PtrType> to a PtrType
  // using the above conversion functions hits an ambiguous resolution bug in
  // clang: http://llvm.org/bugs/show_bug.cgi?id=18359 While that exists, we can
  // use these as simple ways of accessing the underlying type (instead of
  // workarounds calling the operators explicitly or adding a constructor call).
  const PtrType& as_nullable() const& { return ptr; }
  PtrType&& as_nullable() && { return std::move(ptr); }

  // Can't convert to bool (that would be silly). The explicit delete results in
  // "value of type 'nn<...>' is not contextually convertible to 'bool'", rather
  // than "no viable conversion", which is a bit more clear.
  operator bool() const = delete;

  // Explicitly deleted constructors. These help produce clearer error messages,
  // as trying to use them will result in clang printing the whole line,
  // including the comment.
  nn(std::nullptr_t) = delete;             // nullptr is not allowed here
  nn& operator=(std::nullptr_t) = delete;  // nullptr is not allowed here

  explicit nn(const PtrType& arg,
              const std::source_location& loc = std::source_location::current())
      : ptr(arg) {
    nn_detail::check(ptr, loc);
  }
  explicit nn(PtrType&& arg,
              const std::source_location& loc = std::source_location::current())
      : ptr(std::move(arg)) {
    nn_detail::check(ptr, loc);
  }

  // Type-converting move and copy constructor. We have four separate cases
  // here, for implicit and explicit move and copy.

  // explicit copy constructor, when PtrType is explicitly constructible but not
  // implicitly convertible from OtherType.
  template <typename OtherType>
    requires std::is_constructible_v<PtrType, OtherType> &&
             (!std::is_convertible_v<OtherType, PtrType>)
  explicit nn(const nn<OtherType>& other)
      : ptr(other.operator const OtherType&()) {}

  // explicit move constructor, when PtrType is explicitly constructible but not
  // implicitly convertible from OtherType, and OtherType is not raw pointer.
  template <typename OtherType>
    requires std::is_constructible_v<PtrType, OtherType> &&
             (!std::is_convertible_v<OtherType, PtrType>) &&
             (!std::is_pointer_v<OtherType>)
  explicit nn(nn<OtherType>&& other)
      : ptr(std::move(other).operator OtherType&&()) {}

  // implicit copy constructor, when PtrType is implicitly convertible from
  // OtherType.
  template <typename OtherType>
    requires std::is_convertible_v<OtherType, PtrType>
  nn(const nn<OtherType>& other) : ptr(other.operator const OtherType&()) {}

  // implicit copy constructor, when PtrType is implicitly convertible from
  // OtherType, and OtherType is not raw pointer.
  template <typename OtherType>
    requires std::is_convertible_v<OtherType, PtrType> &&
             (!std::is_pointer_v<OtherType>)
  nn(nn<OtherType>&& other) : ptr(std::move(other).operator OtherType&&()) {}

  // A type-converting move and copy assignment operator aren't necessary;
  // writing "base_ptr = derived_ptr;" will run the type-converting constructor
  // followed by the implicit move assignment operator.

  // Two-argument constructor, designed for use with the shared_ptr aliasing
  // constructor. This will not be instantiated if PtrType doesn't have a
  // suitable constructor.
  template <typename OtherType>
    requires std::is_constructible_v<PtrType, OtherType, element_type*>
  nn(const nn<OtherType>& ownership_ptr, nn<element_type*> target_ptr)
      : ptr(ownership_ptr.operator const OtherType&(), target_ptr) {}

  // Comparisons. Other comparisons are implemented in terms of these.
  template <typename L, typename R>
  friend bool operator==(const nn<L>&, const R&);
  template <typename L, typename R>
  friend bool operator==(const L&, const nn<R>&);
  template <typename L, typename R>
  friend bool operator==(const nn<L>&, const nn<R>&);

  template <typename L, typename R>
  friend bool operator<(const nn<L>&, const R&);
  template <typename L, typename R>
  friend bool operator<(const L&, const nn<R>&);
  template <typename L, typename R>
  friend bool operator<(const nn<L>&, const nn<R>&);

  // ostream operator
  template <typename T>
  friend std::ostream& operator<<(std::ostream&, const nn<T>&);

  template <typename T = PtrType>
  element_type* get() const {
    return ptr.get();
  }

 private:
  // Backing pointer
  PtrType ptr;
};

// Base comparisons - these are friends of nn<PtrType>, so they can access .ptr
// directly.
template <typename L, typename R>
bool operator==(const nn<L>& l, const R& r) {
  return l.ptr == r;
}
template <typename L, typename R>
bool operator==(const L& l, const nn<R>& r) {
  return l == r.ptr;
}
template <typename L, typename R>
bool operator==(const nn<L>& l, const nn<R>& r) {
  return l.ptr == r.ptr;
}
template <typename L, typename R>
bool operator<(const nn<L>& l, const R& r) {
  return l.ptr < r;
}
template <typename L, typename R>
bool operator<(const L& l, const nn<R>& r) {
  return l < r.ptr;
}
template <typename L, typename R>
bool operator<(const nn<L>& l, const nn<R>& r) {
  return l.ptr < r.ptr;
}
template <typename T>
std::ostream& operator<<(std::ostream& os, const nn<T>& p) {
  return os << p.ptr;
}

#define NN_DERIVED_OPERATORS(op, base)               \
  template <typename L, typename R>                  \
  bool operator op(const nn<L>& l, const R& r) {     \
    return base;                                     \
  }                                                  \
  template <typename L, typename R>                  \
  bool operator op(const L& l, const nn<R>& r) {     \
    return base;                                     \
  }                                                  \
  template <typename L, typename R>                  \
  bool operator op(const nn<L>& l, const nn<R>& r) { \
    return base;                                     \
  }

NN_DERIVED_OPERATORS(>, r < l)
NN_DERIVED_OPERATORS(<=, !(l > r))
NN_DERIVED_OPERATORS(>=, !(l < r))
NN_DERIVED_OPERATORS(!=, !(l == r))

#undef NN_DERIVED_OPERATORS

// Convenience typedefs
template <typename T>
using nn_unique_ptr = nn<std::unique_ptr<T>>;
template <typename T>
using nn_shared_ptr = nn<std::shared_ptr<T>>;

template <typename T, typename... Args>
nn_unique_ptr<T> nn_make_unique(Args&&... args) {
  return nn_unique_ptr<T>(std::make_unique<T>(std::forward<Args>(args)...));
}

template <typename T, typename... Args>
nn_shared_ptr<T> nn_make_shared(Args&&... args) {
  return nn_shared_ptr<T>(std::make_shared<T>(std::forward<Args>(args)...));
}

template <typename T>
class nn_enable_shared_from_this : public std::enable_shared_from_this<T> {
 public:
  using std::enable_shared_from_this<T>::enable_shared_from_this;
  nn_shared_ptr<T> nn_shared_from_this() {
    return nn_shared_ptr<T>(this->shared_from_this());
  }
  nn_shared_ptr<const T> nn_shared_from_this() const {
    return nn_shared_ptr<const T>(this->shared_from_this());
  }
};

template <typename T>
nn<T*> nn_addr(T& object) {
  return nn<T*>(&object);
}

template <typename T>
nn<const T*> nn_addr(const T& object) {
  return nn<const T*>(&object);
}

/* Non-nullable equivalents of shared_ptr's specialized casting functions.
 * These convert through a shared_ptr since nn<shared_ptr<T>> lacks the
 * ref-count-sharing cast constructor, but thanks to moves there shouldn't be
 * any significant extra cost. */
template <typename T, typename U>
nn_shared_ptr<T> nn_static_pointer_cast(const nn_shared_ptr<U>& org_ptr) {
  auto raw_ptr =
      static_cast<typename nn_shared_ptr<T>::element_type*>(org_ptr.get());
  std::shared_ptr<T> nullable_ptr(org_ptr.as_nullable(), raw_ptr);
  return nn_shared_ptr<T>(std::move(nullable_ptr));
}

template <typename T, typename U>
std::shared_ptr<T> nn_dynamic_pointer_cast(const nn_shared_ptr<U>& org_ptr) {
  auto raw_ptr =
      dynamic_cast<typename std::shared_ptr<T>::element_type*>(org_ptr.get());
  if (!raw_ptr) {
    return nullptr;
  } else {
    return std::shared_ptr<T>(org_ptr.as_nullable(), raw_ptr);
  }
}

template <typename T, typename U>
nn_shared_ptr<T> nn_const_pointer_cast(const nn_shared_ptr<U>& org_ptr) {
  auto raw_ptr =
      const_cast<typename nn_shared_ptr<T>::element_type*>(org_ptr.get());
  std::shared_ptr<T> nullable_ptr(org_ptr.as_nullable(), raw_ptr);
  return nn_shared_ptr<T>(std::move(nullable_ptr));
}

}  // namespace util

namespace std {

template <typename T>
struct hash<::util::nn<T>> {
  size_t operator()(const ::util::nn<T>& obj) const {
    return std::hash<T>{}(obj.as_nullable());
  }
};

}  // namespace std
