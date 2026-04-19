"""
Economics analytics package.

Imports are intentionally lazy. Several analytics modules have optional heavy
dependencies such as matplotlib; importing the package must not break CLI tools
that only need one lightweight analyzer.
"""

from .core import (
    CalculationError,
    DataError,
    DataValidator,
    EconomicsBase,
    EconomicsError,
    ValidationError,
)

__version__ = "1.0.0"
__author__ = "Fincept Corporation"
__email__ = "dev@fincept.com"

_LAZY_EXPORTS = {
    "CurrencyAnalyzer": ".currency_analysis",
    "SpotForwardAnalyzer": ".currency_analysis",
    "ArbitrageDetector": ".currency_analysis",
    "ParityAnalyzer": ".currency_analysis",
    "CarryTradeAnalyzer": ".currency_analysis",
    "ExchangeCalculator": ".exchange_calculations",
    "CrossRateCalculator": ".exchange_calculations",
    "ForwardCalculator": ".exchange_calculations",
    "GrowthAnalyzer": ".growth_analysis",
    "ProductivityAnalyzer": ".growth_analysis",
    "ConvergenceAnalyzer": ".growth_analysis",
    "DemographicAnalyzer": ".growth_analysis",
    "BusinessCycleAnalyzer": ".market_cycles",
    "MarketStructureAnalyzer": ".market_cycles",
    "CreditCycleAnalyzer": ".market_cycles",
    "FiscalPolicyAnalyzer": ".policy_analysis",
    "MonetaryPolicyAnalyzer": ".policy_analysis",
    "CentralBankAnalyzer": ".policy_analysis",
    "TradeAnalyzer": ".trade_geopolitics",
    "GeopoliticalRiskAnalyzer": ".trade_geopolitics",
    "TradingBlocAnalyzer": ".trade_geopolitics",
    "CapitalFlowAnalyzer": ".capital_flows",
    "FXMarketAnalyzer": ".capital_flows",
    "ExchangeRegimeAnalyzer": ".capital_flows",
    "DataHandler": ".data_handler",
    "DataProvider": ".data_handler",
    "ManualDataInput": ".data_handler",
    "StatisticalAnalyzer": ".analytics_engine",
    "ForecastingEngine": ".analytics_engine",
    "ScenarioAnalyzer": ".analytics_engine",
    "ReportGenerator": ".reporting",
    "VisualizationEngine": ".reporting",
    "ExportManager": ".reporting",
    "EconomicsConfig": ".config",
    "DataSources": ".config",
    "CalculationPrecision": ".config",
}

__all__ = [
    "EconomicsBase",
    "DataValidator",
    "EconomicsError",
    "ValidationError",
    "CalculationError",
    "DataError",
    *_LAZY_EXPORTS.keys(),
]


def __getattr__(name):
    module_name = _LAZY_EXPORTS.get(name)
    if not module_name:
        raise AttributeError(f"module {__name__!r} has no attribute {name!r}")

    import importlib

    module = importlib.import_module(module_name, __name__)
    value = getattr(module, name)
    globals()[name] = value
    return value


def analyze_currency_arbitrage(currency_data, base_currency="USD"):
    """Quick triangular arbitrage analysis."""
    detector = __getattr__("ArbitrageDetector")()
    return detector.detect_triangular_arbitrage(currency_data, base_currency)


def calculate_gdp_growth(gdp_data, method="solow"):
    """Quick GDP growth decomposition."""
    analyzer = __getattr__("GrowthAnalyzer")()
    return analyzer.decompose_growth(gdp_data, method)


def assess_policy_impact(policy_data, policy_type="fiscal"):
    """Quick policy impact assessment."""
    analyzer_cls = __getattr__("FiscalPolicyAnalyzer" if policy_type == "fiscal" else "MonetaryPolicyAnalyzer")
    return analyzer_cls().assess_impact(policy_data)


def detect_business_cycle_phase(economic_indicators):
    """Quick business cycle phase detection."""
    analyzer = __getattr__("BusinessCycleAnalyzer")()
    return analyzer.detect_phase(economic_indicators)


DEFAULT_CONFIG = {
    "precision": 8,
    "base_currency": "USD",
    "data_validation": True,
    "error_tolerance": 1e-6,
    "default_forecast_periods": 12,
    "confidence_interval": 0.95,
}


def configure_module(**kwargs):
    """Configure module-wide settings."""
    config = __getattr__("EconomicsConfig")()
    for key, value in kwargs.items():
        if hasattr(config, key):
            setattr(config, key, value)
    return config
