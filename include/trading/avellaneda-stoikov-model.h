#ifndef AVELLANEDA_STOIKOV_MODEL_H
#define AVELLANEDA_STOIKOV_MODEL_H

#include <cmath>
#include <algorithm>

namespace MarketMaker {

// Result of Avellaneda-Stoikov quote computation
struct ASQuoteResult {
    double bid_price;
    double ask_price;
    double reservation_price;  // Inventory-adjusted fair price
    double optimal_spread;     // Volatility-adjusted spread
};

// Avellaneda-Stoikov inventory-aware market making model.
// Adjusts quotes based on inventory risk and market volatility.
// Reference: Avellaneda & Stoikov (2008) "High-frequency trading in a limit order book"
class AvellanedaStoikovModel {
public:
    // gamma: risk aversion parameter (higher = wider spreads, faster inventory reduction)
    // kappa: order arrival intensity (higher = tighter spreads, more aggressive quoting)
    // time_horizon_sec: rolling time window in seconds (spread narrows as t -> T)
    AvellanedaStoikovModel(double gamma = 0.001, double kappa = 1.5,
                           double time_horizon_sec = 300.0)
        : gamma_(std::max(gamma, 1e-9))  // Prevent division by zero
        , kappa_(std::max(kappa, 1e-9))
        , time_horizon_sec_(std::max(time_horizon_sec, 1.0)) {}

    // Compute optimal bid/ask quotes.
    // mid_price: current market mid price
    // inventory: net position (positive = long, negative = short)
    // volatility: current price volatility (standard deviation)
    // time_remaining_sec: seconds until time horizon reset (0 = at horizon)
    ASQuoteResult compute(double mid_price, double inventory,
                          double volatility, double time_remaining_sec) const {
        // Clamp time_remaining to avoid division issues at horizon boundary
        double tau = std::max(time_remaining_sec / time_horizon_sec_, 0.001);

        double sigma_sq = volatility * volatility;

        // Reservation price: adjusted mid that accounts for inventory risk
        // r(s,q,t) = s - q * gamma * sigma^2 * tau
        // When long (q>0): reservation price drops below mid (want to sell)
        // When short (q<0): reservation price rises above mid (want to buy)
        double reservation_price = mid_price - inventory * gamma_ * sigma_sq * tau;

        // Optimal spread: widens with volatility, narrows near horizon end
        // delta = gamma * sigma^2 * tau + (2/gamma) * ln(1 + gamma/kappa)
        double optimal_spread = gamma_ * sigma_sq * tau
                              + (2.0 / gamma_) * std::log(1.0 + gamma_ / kappa_);

        // Ensure spread is at least a minimal amount (prevent crossed orders)
        double half_spread = std::max(optimal_spread / 2.0, mid_price * 1e-6);

        return ASQuoteResult{
            reservation_price - half_spread,  // bid
            reservation_price + half_spread,  // ask
            reservation_price,
            optimal_spread
        };
    }

    // Getters for parameters (useful for logging/debugging)
    double gamma() const { return gamma_; }
    double kappa() const { return kappa_; }
    double time_horizon_sec() const { return time_horizon_sec_; }

private:
    double gamma_;
    double kappa_;
    double time_horizon_sec_;
};

} // namespace MarketMaker

#endif // AVELLANEDA_STOIKOV_MODEL_H
