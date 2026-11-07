// ============================================================================
//  pooriayousefi/utility/expected.hpp — Expected<Value, Error> Result Type
//  Project: mcpxx | C++20 | Apache-2.0
//
//  Stores an empty, value, or error state. Set it explicitly with set_value()
//  or set_error(), then inspect it through the observer and accessor methods.
// ============================================================================

#pragma once

#include <utility>
#include <variant>

namespace pooriayousefi
{
    struct ExpectedSuccess {};

    constexpr ExpectedSuccess expected_success() { return ExpectedSuccess{}; }

    template <typename Value, typename Error>
    class Expected
    {
    public:
        using value_type = Value;
        using error_type = Error;

    private:
        std::variant<std::monostate, value_type, error_type> storage_;

    public:
        // --- constructors --------------------------------------------------

        // Default constructor: empty state: neither a value nor an error has been stored yet.
        Expected() :storage_{} {}

        // --- copy constructor ----------------------------------------------

        Expected(const Expected& other) :storage_(other.storage_){}

        // --- move constructor ----------------------------------------------

        Expected(Expected&& other) noexcept :storage_{std::exchange(other.storage_, std::monostate{})}{}

        // --- assignment operators ------------------------------------------

        // Copy assignment operator
        Expected& operator=(const Expected& other)
        {
            storage_ = other.storage_;
            return *this;
        }

        // Move assignment operator
        Expected& operator= (Expected&& other) noexcept
        {
            std::swap(storage_, other.storage_);
            return *this;
        }

        // --- setters -------------------------------------------------------

        inline void set_value(const value_type& value)
        {
            storage_.template emplace<1>(value);
        }

        inline void set_value(value_type&& value)
        {
            storage_.template emplace<1>(std::move(value));
        }

        inline void set_error(const error_type& error)
        {
            storage_.template emplace<2>(error);
        }

        inline void set_error(error_type&& error)
        {
            storage_.template emplace<2>(std::move(error));
        }

        // --- observers -----------------------------------------------------
        // value() and error() throw std::bad_variant_access when the side they
        // return is not the one currently stored.

        bool has_value() const
        {
            return storage_.index() == 1;
        }

        bool has_error() const
        {
            return storage_.index() == 2;
        }

        explicit operator bool() const
        {
            return has_value();
        }

        // --- getters -------------------------------------------------------

        const value_type& get_value() const
        {
            return std::get<1>(storage_);
        }

        value_type get_value()
        {
            return std::get<1>(storage_);
        }

        const error_type& get_error() const
        {
            return std::get<2>(storage_);
        }

        error_type get_error()
        {
            return std::get<2>(storage_);
        }
    };

    // Void specialization: carries either a successful "nothing" or an error.
    // Default state is empty, matching the primary template; mark success
    // explicitly with expected_success.
    template <typename Error>
    class Expected<void, Error>
    {
    public:
        using value_type = ExpectedSuccess;
        using error_type = Error;

    private:
        std::variant<std::monostate, value_type, error_type> storage_;

    public:
        // --- constructors --------------------------------------------------

        // Default constructor: empty state: neither a value nor an error has been stored yet.
        Expected() :storage_{}{}

        // --- copy constructor ----------------------------------------------

        Expected(const Expected& other) :storage_(other.storage_){}

        // --- move constructor ----------------------------------------------

        Expected(Expected&& other) noexcept :storage_{std::exchange(other.storage_, std::monostate{})}{}

        // --- assignment operators ------------------------------------------

        // Copy assignment operator
        Expected& operator=(const Expected& other)
        {
            storage_ = other.storage_;
            return *this;
        }

        // Move assignment operator
        Expected& operator= (Expected&& other) noexcept
        {
            std::swap(storage_, other.storage_);
            return *this;
        }

        // --- setters -------------------------------------------------------

        inline void set_value()
        {
            storage_.template emplace<1>(value_type{});
        }

        inline void set_value(const value_type& value)
        {
            storage_.template emplace<1>(value);
        }

        inline void set_value(value_type&& value)
        {
            storage_.template emplace<1>(std::move(value));
        }

        inline void set_error(const error_type& error)
        {
            storage_.template emplace<2>(error);
        }

        inline void set_error(error_type&& error)
        {
            storage_.template emplace<2>(std::move(error));
        }

        // --- observers -----------------------------------------------------
        // value() and error() throw std::bad_variant_access when the side they
        // return is not the one currently stored.

        bool has_value() const
        {
            return storage_.index() == 1;
        }

        bool has_error() const
        {
            return storage_.index() == 2;
        }

        explicit operator bool() const
        {
            return has_value();
        }

        // --- getters -------------------------------------------------------

        const value_type& get_value() const
        {
            return std::get<1>(storage_);
        }

        value_type get_value()
        {
            return std::get<1>(storage_);
        }

        const error_type& get_error() const
        {
            return std::get<2>(storage_);
        }

        error_type get_error()
        {
            return std::get<2>(storage_);
        }
    };
}