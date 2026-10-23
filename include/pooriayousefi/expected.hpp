
#pragma once

#include <type_traits>
#include <utility>
#include <variant>

namespace pooriayousefi
{
    struct ExpectedSuccess {};
    constexpr ExpectedSuccess expected_success() noexcept { return ExpectedSuccess{}; }

    template <typename Error>
    class UnexpectedErrorWrapper
    {
    private:
        Error error_;

    public:
        UnexpectedErrorWrapper() = delete;
        explicit UnexpectedErrorWrapper(Error&& error) noexcept :error_{std::move(error)}{}
        inline const Error& unwrap() const { return error_; }
        inline Error& unwrap() { return error_; }
    };

    template <typename Value, typename Error>
    class Expected
    {
    private:
        std::variant<std::monostate, Value, UnexpectedErrorWrapper<Error>> value_;
    
    public:
        Expected() :value_{}{}
        explicit Expected(Value&& value) :value_{std::in_place_index<1>, std::move(value)}{}
        explicit Expected(Error&& error) :value_{std::in_place_index<2>, std::move(error)}{}
        Expected(const Expected& other) :value_{other.value_}{}
        Expected(Expected&& other) noexcept :value_{std::exchange(other.value_, std::monostate{})}{}
        inline Expected& operator=(const Expected& other) 
        {
            value_ = other.value_; 
            return *this;
        }
        inline Expected& operator=(Expected&& other) noexcept 
        {
            std::swap(value_, other.value_); 
            other.value_ = std::monostate{}; 
            return *this;
        }
        inline Expected& operator=(Value&& value)
        {
            value_.template emplace<1>(std::move(value));
            return *this;
        }
        inline Expected& operator=(Error&& error)
        {
            value_.template emplace<2>(std::move(error));
            return *this;
        }
        inline bool has_value() const noexcept {return value_.index() == 1;}
        inline bool has_error() const noexcept {return value_.index() == 2;}
        inline bool empty() const noexcept {return value_.index() == 0;}
        inline const Value& value() const {return std::get<1>(value_);}
        inline Value& value() {return std::get<1>(value_);}
        inline const Error& error() const {return std::get<2>(value_).unwrap();}
        inline Error& error() {return std::get<2>(value_).unwrap();}
    };
    template <typename Error>
    class Expected<void, Error>
    {
    private:
        std::variant<std::monostate, ExpectedSuccess, UnexpectedErrorWrapper<Error>> value_;
    
    public:
        Expected() :value_{}{}
        explicit Expected(ExpectedSuccess&& value) :value_{std::in_place_index<1>, std::move(value)}{}
        explicit Expected(Error&& error) :value_{std::in_place_index<2>, std::move(error)}{}
        Expected(const Expected& other) :value_{other.value_}{}
        Expected(Expected&& other) noexcept :value_{std::exchange(other.value_, std::monostate{})}{}
        inline Expected& operator=(const Expected& other) 
        {
            value_ = other.value_; 
            return *this;
        }
        inline Expected& operator=(Expected&& other) noexcept 
        {
            std::swap(value_, other.value_); 
            other.value_ = std::monostate{}; 
            return *this;
        }
        inline Expected& operator=(ExpectedSuccess&& value)
        {
            value_.template emplace<1>(std::move(value));
            return *this;
        }
        inline Expected& operator=(Error&& error)
        {
            value_.template emplace<2>(std::move(error));
            return *this;
        }
        inline bool has_value() const noexcept {return value_.index() == 1;}
        inline bool has_error() const noexcept {return value_.index() == 2;}
        inline bool empty() const noexcept {return value_.index() == 0;}
        inline const ExpectedSuccess& value() const {return std::get<1>(value_);}
        inline ExpectedSuccess& value() {return std::get<1>(value_);}
        inline const Error& error() const {return std::get<2>(value_).unwrap();}
        inline Error& error() {return std::get<2>(value_).unwrap();}
    };
}