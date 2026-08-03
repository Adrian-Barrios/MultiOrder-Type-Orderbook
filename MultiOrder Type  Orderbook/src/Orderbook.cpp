#include <iostream>
#include <map>
#include <set>
#include <list>
#include <cmath>
#include <ctime>
#include <deque>
#include <queue>
#include <stack>
#include <limits>
#include <string>
#include <vector>
#include <numeric>
#include <algorithm>
#include <unordered_map>
#include <memory>
#include <variant>
#include <optional>
#include <tuple>
#include <format>

enum class OrderType
{
	GoodTillCancel,
	FillAndKill
};

enum class Side
{
	Buy,
	Sell
};

using Price = std::int32_t;
using Quantity = std::uint32_t;
using OrderId = std::uint64_t;

struct LevelInfo
{
	Price price_;
	Quantity quantity_;
};

using LevelInfos = std::vector<LevelInfo>;

class OrderbookLevelInfos
{
public:
	OrderbookLevelInfos(const LevelInfos& bids, const LevelInfos& asks)
		: bids_{ bids }
		, asks_{ asks }
	{}
	const LevelInfos& GetBids() const { return bids_; }
	const LevelInfos& GetAsks() const { return asks_; }
private:
	LevelInfos bids_;
	LevelInfos asks_;
};

class Order
{
public:
	Order(OrderType orderType, OrderId orderId, Side side, Price price, Quantity quantity)
		: orderType_{ orderType }
		, orderId_{ orderId }
		, side_{ side }
		, price_{ price }
		, initialQuantity_{ quantity }
		, remainingQuantity_ { quantity }
	{ }
	OrderId GetOrderId() const { return orderId_; }
	Side GetSide() const { return side_; }
	Price GetPrice() const { return price_; }
	OrderType GetOrderType() const { return orderType_; }
	Quantity GetIntitialQuantity() const { return initialQuantity_; }
	Quantity GetRemainingQuantity() const { return remainingQuantity_; }
	Quantity GetFilledQuantity() const { return GetIntitialQuantity() - GetRemainingQuantity(); }
	bool isFilled() const { return GetRemainingQuantity() == 0; }
	void Fill(Quantity quantity)
	{
		if (quantity > GetRemainingQuantity())
			throw std::logic_error(std::format("Order ({}) cannot be filled for more than its remaining quantity", GetOrderId()));
		remainingQuantity_ -= quantity;
	}

private:
	OrderType orderType_;
	OrderId orderId_;
	Side side_;
	Price price_;
	Quantity initialQuantity_;
	Quantity remainingQuantity_;
};

using OrderPointer = std::shared_ptr<Order>; // Because an order can be stored in an Orders dictionary and bid or ask dictionary as well.
using OrderPointers = std::list<OrderPointer>; // List instead of vector so pointers are not invalidated.

class OrderModify // Represents a request to modify an existing order (change price and/or quantity)
{
public:
	OrderModify(OrderId orderId, Side side, Price price, Quantity quantity)
		: orderId_{ orderId }
		, price_{ price }
		, side_{ side }
		, quantity_{ quantity }
	{}

	OrderId GetOrderId() const { return orderId_; }
	Price GetPrice() const { return price_; }
	Side GetSide() const { return side_;  }
	Quantity GetQuantity() const { return quantity_; }

	OrderPointer ToOrderPointer(OrderType type) const
	{
		return std::make_shared<Order>(type, GetOrderId(), GetSide(), GetPrice(), GetQuantity());
	}
private:
	OrderId orderId_;
	Price price_;
	Side side_;
	Quantity quantity_;
};

struct TradeInfo
{
	OrderId orderId_;
	Price price_;
	Quantity quantity_;
};

class Trade // Caputres both a sell and buy order.
{
public:
	Trade(const TradeInfo& bidTrade, const TradeInfo& askTrade)
		: bidTrade_{ bidTrade }
		, askTrade_{ askTrade }
	{ }
	const TradeInfo& GetBidTrade() const { return bidTrade_; }
	const TradeInfo& GetAskTrade() const { return askTrade_; }
private:
	TradeInfo bidTrade_;
	TradeInfo askTrade_;
};

using Trades = std::vector<Trade>;

// Orderbook

class OrderBook
{
private:
	struct OrderEntry
	{
		OrderPointer order_{ nullptr };
		OrderPointers::iterator location_;
	};

	std::map<Price, OrderPointers, std::greater<Price>> bids_; // The best bid is the one willing to pay th most.
	std::map<Price, OrderPointers, std::less<Price>> asks_; // The best ask is th cheapest sell offer.
	std::unordered_map<OrderId, OrderEntry> orders_;

	bool CanMatch(Side side, Price price) const
	{
		if (side == Side::Buy)
		{
			if (asks_.empty())
				return false;
			const auto& [bestAsk, _] = *asks_.begin();
			return price >= bestAsk;
		}
		else
		{
			if (bids_.empty())
				return false;

			const auto& [bestBid, _] = *bids_.begin();
			return price <= bestBid;
		}
	}

	Trades MatchOrders()
	{
		Trades trades;
		trades.reserve(orders_.size());

		while (true)
		{
			if (bids_.empty() || asks_.empty())
				break;
			auto& [bidPrice, bids] = *bids_.begin();
			auto& [askPrice, asks] = *asks_.begin();

			if (bidPrice < askPrice)
				break;

			while (bids.size() && asks.size())
			{
				auto& bid = bids.front();
				auto& ask = asks.front();

				Quantity quantity = std::min(bid->GetRemainingQuantity(), ask->GetRemainingQuantity());

				bid->Fill(quantity);
				ask->Fill(quantity);

				if (bid->isFilled())
				{
					bids.pop_front();
					orders_.erase(bid->GetOrderId());
				}
				if (ask->isFilled())
				{
					asks.pop_front();
					orders_.erase(ask->GetOrderId());
				}

				if (bids.empty())
					bids_.erase(bidPrice);

				if (asks.empty())
					asks_.erase(askPrice);

				trades.push_back(Trade{
					TradeInfo{ bid->GetOrderId(), bid->GetPrice(), quantity}
					, TradeInfo {ask->GetOrderId(), ask->GetPrice(), quantity}
					});

			}
		}

		if (!bids_.empty())
		{
			auto& [_, bids] = *bids_.begin();
			auto& order = bids.front();
			if (order->GetOrderType() == OrderType::FillAndKill)
				CancelOrder(order->GetOrderId());
		}

		if (!asks_.empty())
		{
			auto& [_, asks] = *asks_.begin();
			auto& order = asks.front();
			if (order->GetOrderType() == OrderType::FillAndKill)
				CancelOrder(order->GetOrderId());
		}
		return trades;

	}

public:

	Trades AddOrder(OrderPointer order)
	{
		if (orders_.contains(order->GetOrderId()))
			return {};

		if (order->GetOrderType() == OrderType::FillAndKill && !CanMatch(order->GetSide(), order->GetPrice()))
			return {};

		OrderPointers::iterator iterator;

		if (order->GetSide() == Side::Buy)
		{
			auto& orders = bids_[order->GetPrice()];
			orders.push_back(order);
			iterator = std::next(orders.begin(), orders.size() - 1);
		}
		else
		{
			auto& orders = asks_[order->GetPrice()];
			orders.push_back(order);
			iterator = std::next(orders.begin(), orders.size() - 1);
		}

		orders_.insert({ order->GetOrderId(), OrderEntry{ order, iterator } });
		return MatchOrders();
	}

	void CancelOrder(OrderId orderId)
	{
		auto it = orders_.find(orderId);
		if (it == orders_.end())
			return;

		// Copy by value — shared_ptr bumps refcount, iterator is just a pointer.
		OrderPointer order = it->second.order_;
		OrderPointers::iterator location = it->second.location_;

		orders_.erase(it);   // now safe

		if (order->GetSide() == Side::Sell)
		{
			auto price = order->GetPrice();
			auto& orders = asks_.at(price);
			orders.erase(location);
			if (orders.empty())
				asks_.erase(price);
		}
		else
		{
			auto price = order->GetPrice();
			auto& orders = bids_.at(price);
			orders.erase(location);
			if (orders.empty())
				bids_.erase(price);
		}
	}

	Trades MatchOrder(OrderModify order)
	{
		auto it = orders_.find(order.GetOrderId());
		if (it == orders_.end())
			return {};

		OrderType existingType = it->second.order_->GetOrderType();  // copy the value out
		CancelOrder(order.GetOrderId());
		return AddOrder(order.ToOrderPointer(existingType));
	}
	std::size_t Size() const { return orders_.size(); }

	OrderbookLevelInfos GetOrderInfos() const
	{
		LevelInfos bidInfos, askInfos;
		bidInfos.reserve(orders_.size());
		askInfos.reserve(orders_.size());

		auto CreateLevelInfos = [](Price price, const OrderPointers& orders)
			{
				return LevelInfo{ price, std::accumulate(orders.begin(), orders.end(), (Quantity)0,
					[](std::size_t runningSum, const OrderPointer& order)
					{return runningSum + order->GetRemainingQuantity();}) };
			};
		for (const auto& [price, orders] : bids_)
			bidInfos.push_back(CreateLevelInfos(price, orders));

		for (const auto& [price, orders] : asks_)
			askInfos.push_back(CreateLevelInfos(price, orders)); 

		return OrderbookLevelInfos{ bidInfos, askInfos };
	}

};

int main()
{
	OrderBook orderbook;
	const OrderId orderId = 1;
	orderbook.AddOrder(std::make_shared<Order>(OrderType::GoodTillCancel, orderId, Side::Buy, 100, 10));
	std::cout << orderbook.Size() << std::endl;
	return 0;
}