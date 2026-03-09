#ifndef EMA_ENGINE_H
#define EMA_ENGINE_H

namespace MarketMaker {

class EmaEngine {
public:
    constexpr explicit EmaEngine(int window)
        : alpha_(2.0 / (window + 1.0)), value_(0.0), initialized_(false) {}

    constexpr void update(double price) {
        if (!initialized_) {
            value_ = price;
            initialized_ = true;
        } else {
            value_ = alpha_ * price + (1.0 - alpha_) * value_;
        }
    }

    [[nodiscard]] constexpr double value() const { return value_; }
    [[nodiscard]] constexpr bool ready() const { return initialized_; }

    constexpr void reset() {
        value_ = 0.0;
        initialized_ = false;
    }

private:
    double alpha_;
    double value_;
    bool initialized_;
};

} // namespace MarketMaker
#endif // EMA_ENGINE_H
