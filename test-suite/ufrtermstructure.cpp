/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 Copyright (C) 2020 Marcin Rybacki

 This file is part of QuantLib, a free-software/open-source library
 for financial quantitative analysts and developers - http://quantlib.org/

 QuantLib is free software: you can redistribute it and/or modify it
 under the terms of the QuantLib license.  You should have received a
 copy of the license along with this program; if not, please email
 <quantlib-dev@lists.sf.net>. The license is also available online at
 <http://quantlib.org/license.shtml>.

 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE.  See the license for more details.
*/

#include "ufrtermstructure.hpp"
#include "utilities.hpp"
#include <ql/currencies/europe.hpp>
#include <ql/indexes/iborindex.hpp>
#include <ql/math/interpolations/loginterpolation.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/termstructures/yield/ufrtermstructure.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/daycounters/simpledaycounter.hpp>

using namespace QuantLib;
using namespace boost::unit_test_framework;

namespace ufr_term_structure_test {

    struct Datum {
        Integer n;
        TimeUnit units;
        Rate rate;
    };

    struct LLFRWeight {
        Time ttm;
        Real weight;
    };

    struct CommonVars {
        // global data
        Date today, settlement;
        Calendar calendar;
        Natural settlementDays;
        Currency ccy;
        BusinessDayConvention businessConvention;
        DayCounter dayCount;
        Frequency fixedFrequency;
        Period floatingTenor;

        ext::shared_ptr<IborIndex> index;
        RelinkableHandle<YieldTermStructure> ftkTermStructureHandle;

        ext::shared_ptr<Quote> ufrRate;
        Time fsp;
        Real alpha;

        // cleanup
        SavedSettings backup;
        // utilities

        CommonVars() {
            settlementDays = 2;
            businessConvention = Unadjusted;
            dayCount = SimpleDayCounter();
            calendar = NullCalendar();
            ccy = EURCurrency();
            fixedFrequency = Annual;
            floatingTenor = 6 * Months;

            index = ext::shared_ptr<IborIndex>(
                new IborIndex("FTK_IDX", floatingTenor, settlementDays, ccy, calendar,
                              businessConvention, false, dayCount, ftkTermStructureHandle));

            // Data source: https://fred.stlouisfed.org/
            Datum swapData[] = {{1, Years, -0.00315}, {2, Years, -0.00205}, {3, Years, -0.00144},
                                {4, Years, -0.00068}, {5, Years, 0.00014},  {6, Years, 0.00103},
                                {7, Years, 0.00194},  {8, Years, 0.00288},  {9, Years, 0.00381},
                                {10, Years, 0.00471}, {12, Years, 0.0063},  {15, Years, 0.00808},
                                {20, Years, 0.00973}, {25, Years, 0.01035}, {30, Years, 0.01055},
                                {40, Years, 0.0103},  {50, Years, 0.0103}};

            InterestRate ufr(0.023, dayCount, Compounded, Annual);
            ufrRate = ext::shared_ptr<Quote>(
                new SimpleQuote(ufr.equivalentRate(Continuous, Annual, 1.0)));
            fsp = 20.0;
            alpha = 0.1;

            today = calendar.adjust(Date(29, March, 2019));
            Settings::instance().evaluationDate() = today;
            settlement = calendar.advance(today, settlementDays, Days);

            Size nInstruments = LENGTH(swapData);
            std::vector<ext::shared_ptr<RateHelper> > instruments(nInstruments);
            for (Size i = 0; i < nInstruments; i++) {
                instruments[i] = ext::shared_ptr<RateHelper>(new SwapRateHelper(
                    swapData[i].rate, Period(swapData[i].n, swapData[i].units), calendar,
                    fixedFrequency, businessConvention, dayCount, index));
            }

            ext::shared_ptr<YieldTermStructure> ftkTermStructure(
                new PiecewiseYieldCurve<Discount, LogLinear>(settlement, instruments, dayCount));
            ftkTermStructure->enableExtrapolation();
            ftkTermStructureHandle.linkTo(ftkTermStructure);
        }
    };

    ext::shared_ptr<Quote> calculateLLFR(const Handle<YieldTermStructure>& ts, Time fsp) {
        DayCounter dc = ts->dayCounter();
        Date ref = ts->referenceDate();
        Real omega = 8.0 / 15.0;

        LLFRWeight llfrWeights[] = {{25.0, 1.0}, {30.0, 0.5}, {40.0, 0.25}, {50.0, 0.125}};
        Size nWeights = LENGTH(llfrWeights);

        Rate llfr = 0.0;
        for (Size j = 0; j < nWeights; j++) {
            LLFRWeight w = llfrWeights[j];
            llfr += w.weight * ts->forwardRate(fsp, w.ttm, Continuous, NoFrequency, true);
        }
        return ext::shared_ptr<Quote>(new SimpleQuote(omega * llfr));
    }

    Rate calculateExtrapolatedForward(const ext::shared_ptr<YieldTermStructure>& ts,
                                      Time t,
                                      Time fsp,
                                      Rate llfr,
                                      Rate ufr,
                                      Real alpha) {
        Time deltaT = t - fsp;
        InterestRate baseRate = ts->zeroRate(fsp, Continuous, NoFrequency, true);
        Real beta = (1.0 - std::exp(-alpha * deltaT)) / (alpha * deltaT);
        return ufr + (llfr - ufr) * beta;
    }
}

void UFRTermStructureTest::testDutchCentralBankRates() {
    BOOST_TEST_MESSAGE("Testing DNB replication of UFR zero annually compounded rates...");

    using namespace ufr_term_structure_test;

    CommonVars vars;

    ext::shared_ptr<Quote> llfr = calculateLLFR(vars.ftkTermStructureHandle, vars.fsp);

    ext::shared_ptr<YieldTermStructure> ufrTs(
        new UFRTermStructure(vars.ftkTermStructureHandle, Handle<Quote>(llfr),
                             Handle<Quote>(vars.ufrRate), vars.fsp, vars.alpha));

    Datum expectedZeroes[] = {{10, Years, 0.00477}, {20, Years, 0.01004}, {30, Years, 0.01223},
                              {40, Years, 0.01433}, {50, Years, 0.01589}, {60, Years, 0.01702},
                              {70, Years, 0.01785}, {80, Years, 0.01849}, {90, Years, 0.01899},
                              {100, Years, 0.01939}};

    Real tolerance = 1.0e-4;
    Size nRates = LENGTH(expectedZeroes);

    for (Size i = 0; i < nRates; ++i) {
        Period p = expectedZeroes[i].n * expectedZeroes[i].units;
        Date maturity = vars.settlement + p;

        Rate actual = ufrTs->zeroRate(maturity, vars.dayCount, Compounded, Annual).rate();
        Rate expected = expectedZeroes[i].rate;

        if (std::fabs(actual - expected) > tolerance)
            BOOST_ERROR("unable to reproduce zero yield rate from the UFR curve\n"
                        << std::setprecision(10) << "    calculated: " << actual << "\n"
                        << "    expected:   " << expected << "\n"
                        << "    tenor:       " << p << "\n");
    }
}

void UFRTermStructureTest::testExtrapolatedForward() {
    BOOST_TEST_MESSAGE("Testing continuous forward rates in extrapolation region...");

    using namespace ufr_term_structure_test;

    CommonVars vars;

    ext::shared_ptr<Quote> llfr(new SimpleQuote(0.0125));

    ext::shared_ptr<YieldTermStructure> ufrTs(
        new UFRTermStructure(vars.ftkTermStructureHandle, Handle<Quote>(llfr),
                             Handle<Quote>(vars.ufrRate), vars.fsp, vars.alpha));

    Period tenors[] = {
        20 * Years, 30 * Years, 40 * Years, 50 * Years,  60 * Years,
        70 * Years, 80 * Years, 90 * Years, 100 * Years,
    };

    Size nTenors = LENGTH(tenors);

    for (Size i = 0; i < nTenors; ++i) {
        Date maturity = vars.settlement + tenors[i];
        Time t = ufrTs->timeFromReference(maturity);

        Rate actual = ufrTs->forwardRate(vars.fsp, t, Continuous, NoFrequency, true).rate();
        Rate expected = calculateExtrapolatedForward(ufrTs, t, vars.fsp, llfr->value(),
                                                     vars.ufrRate->value(), vars.alpha);

        Real tolerance = 1.0e-10;
        if (std::fabs(actual - expected) > tolerance)
            BOOST_ERROR("unable to replicate the forward rate from the UFR curve\n"
                        << std::setprecision(10) << "    calculated: " << actual << "\n"
                        << "    expected:   " << expected << "\n"
                        << "    tenor:       " << tenors[i] << "\n");
    }
}

void UFRTermStructureTest::testZeroRateAtFirstSmoothingPoint() {
    BOOST_TEST_MESSAGE("Testing zero rate on the First Smoothing Point...");

    using namespace ufr_term_structure_test;

    CommonVars vars;

    ext::shared_ptr<Quote> llfr(new SimpleQuote(0.0125));

    ext::shared_ptr<YieldTermStructure> ufrTs(
        new UFRTermStructure(vars.ftkTermStructureHandle, Handle<Quote>(llfr),
                             Handle<Quote>(vars.ufrRate), vars.fsp, vars.alpha));

    Rate actual = ufrTs->zeroRate(vars.fsp, Continuous, NoFrequency, true).rate();
    Rate expected =
        vars.ftkTermStructureHandle->zeroRate(vars.fsp, Continuous, NoFrequency, true).rate();

    Real tolerance = 1.0e-10;
    if (std::fabs(actual - expected) > tolerance)
        BOOST_ERROR("unable to replicate the zero rate on the First Smoothing Point\n"
                    << std::setprecision(10) << "    calculated: " << actual << "\n"
                    << "    expected:   " << expected << "\n"
                    << "    FSP:       " << vars.fsp << "\n");
}

void UFRTermStructureTest::testThatInspectorsEqualToBaseCurve() {
    BOOST_TEST_MESSAGE("Testing UFR curve inspectors...");

    using namespace ufr_term_structure_test;

    CommonVars vars;

    ext::shared_ptr<Quote> llfr(new SimpleQuote(0.0125));

    ext::shared_ptr<YieldTermStructure> ufrTs(
        new UFRTermStructure(vars.ftkTermStructureHandle, Handle<Quote>(llfr),
                             Handle<Quote>(vars.ufrRate), vars.fsp, vars.alpha));

    if (ufrTs->dayCounter() != vars.ftkTermStructureHandle->dayCounter())
        BOOST_ERROR("different day counter on the UFR curve than on the base curve\n"
                    << std::setprecision(10) << "    UFR curve: " << ufrTs->dayCounter() << "\n"
                    << "    base curve:   " << vars.ftkTermStructureHandle->dayCounter() << "\n");

    if (ufrTs->calendar() != vars.ftkTermStructureHandle->calendar())
        BOOST_ERROR("different calendar on the UFR curve than on the base curve\n"
                    << std::setprecision(10) << "    UFR curve: " << ufrTs->calendar() << "\n"
                    << "    base curve:   " << vars.ftkTermStructureHandle->calendar() << "\n");

    if (ufrTs->referenceDate() != vars.ftkTermStructureHandle->referenceDate())
        BOOST_ERROR("different reference date on the UFR curve than on the base curve\n"
                    << std::setprecision(10) << "    UFR curve: " << ufrTs->referenceDate() << "\n"
                    << "    base curve:   " << vars.ftkTermStructureHandle->referenceDate()
                    << "\n");

    if (ufrTs->maxDate() != vars.ftkTermStructureHandle->maxDate())
        BOOST_ERROR("different max date on the UFR curve than on the base curve\n"
                    << std::setprecision(10) << "    UFR curve: " << ufrTs->maxDate() << "\n"
                    << "    base curve:   " << vars.ftkTermStructureHandle->maxDate() << "\n");

    if (ufrTs->maxTime() != vars.ftkTermStructureHandle->maxTime())
        BOOST_ERROR("different max time on the UFR curve than on the base curve\n"
                    << std::setprecision(10) << "    UFR curve: " << ufrTs->maxTime() << "\n"
                    << "    base curve:   " << vars.ftkTermStructureHandle->maxTime() << "\n");
}

void UFRTermStructureTest::testExceptionWhenFspLessOrEqualZero() {
    BOOST_TEST_MESSAGE("Testing exception when the First Smoothing Point less or equal zero...");

    using namespace ufr_term_structure_test;

    CommonVars vars;

    ext::shared_ptr<Quote> llfr(new SimpleQuote(0.0125));

    ext::shared_ptr<YieldTermStructure> ufrTs(
        new UFRTermStructure(vars.ftkTermStructureHandle, Handle<Quote>(llfr),
                             Handle<Quote>(vars.ufrRate), vars.fsp, vars.alpha));

    BOOST_CHECK_THROW(ext::shared_ptr<YieldTermStructure> ufrTs(
                          new UFRTermStructure(vars.ftkTermStructureHandle, Handle<Quote>(llfr),
                                               Handle<Quote>(vars.ufrRate), 0.0, vars.alpha)),
                      Error);

    BOOST_CHECK_THROW(ext::shared_ptr<YieldTermStructure> ufrTs(
                          new UFRTermStructure(vars.ftkTermStructureHandle, Handle<Quote>(llfr),
                                               Handle<Quote>(vars.ufrRate), -1.0, vars.alpha)),
                      Error);
}

test_suite* UFRTermStructureTest::suite() {
    test_suite* suite = BOOST_TEST_SUITE("UFR term structure tests");

    suite->add(QUANTLIB_TEST_CASE(&UFRTermStructureTest::testDutchCentralBankRates));
    suite->add(QUANTLIB_TEST_CASE(&UFRTermStructureTest::testExtrapolatedForward));
    suite->add(QUANTLIB_TEST_CASE(&UFRTermStructureTest::testZeroRateAtFirstSmoothingPoint));
    suite->add(QUANTLIB_TEST_CASE(&UFRTermStructureTest::testThatInspectorsEqualToBaseCurve));
    suite->add(QUANTLIB_TEST_CASE(&UFRTermStructureTest::testExceptionWhenFspLessOrEqualZero));

    return suite;
}