# Copyright 2015-2026 the openage authors. See copying.md for legal info.

include(CheckCXXSourceRuns)

if(cxx_thread_local IN_LIST CMAKE_CXX_COMPILE_FEATURES)
	set(HAVE_THREAD_LOCAL_STORAGE true)
else()
	set(HAVE_THREAD_LOCAL_STORAGE false)
endif()

set(CXX20_TEST_SOURCE "
#include <compare>
#include <concepts>

class Cat {
public:
	virtual int maunz() const { return 42; };

	auto operator <=>(const Cat &other) const = default;
};

template <typename T>
concept Meowable = std::same_as<T, Cat> || std::derived_from<T, Cat>;

template <Meowable T>
int meow(const T &cat) {
	return cat.maunz();
}

class HouseCat : public Cat {
public:
	int maunz() const override { return 1337; };
};

int main() {
	HouseCat mrr{};
	Cat rrarrr{};
	return meow(mrr) > meow(rrarrr) ? 0 : 1;
}
")

# XR fork: a cross build (e.g. Android) cannot run the test program, compiling it is enough
if(CMAKE_CROSSCOMPILING)
	include(CheckCXXSourceCompiles)
	check_cxx_source_compiles("${CXX20_TEST_SOURCE}" HAVE_REQUIRED_CXX20_SUPPORT)
else()
	check_cxx_source_runs("${CXX20_TEST_SOURCE}" HAVE_REQUIRED_CXX20_SUPPORT)
endif()

if(NOT HAVE_REQUIRED_CXX20_SUPPORT)
	message("

The compiler doesn't support required C++20 features:
  * Concepts
  * Default comparisons
The following versions support these features:
  * clang++ >= 10
  * g++ >= 10
  * Microsoft Visual Studio 2019 >= 16.8
Please upgrade your compiler to build openage.

")
	message(FATAL_ERROR "aborting")
endif()
