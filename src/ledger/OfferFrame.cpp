// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "ledger/OfferFrame.h"
#include "database/Database.h"
#include "crypto/Base58.h"
#include "crypto/SHA.h"
#include "lib/json/json.h"
#include "LedgerDelta.h"
#include "util/types.h"

using namespace std;
using namespace soci;

namespace stellar
{
    const char *OfferFrame::kSQLCreateStatement = 
        "CREATE TABLE IF NOT EXISTS Offers                     \
         (                                                     \
         accountID       CHARACTER(64)  NOT NULL,              \
         sequence        INT            NOT NULL               \
                                        CHECK (sequence >= 0), \
         paysIsoCurrency CHARACTER(4)   NOT NULL,              \
         paysIssuer      CHARACTER(64)  NOT NULL,              \
         getsIsoCurrency CHARACTER(4)   NOT NULL,              \
         getsIssuer      CHARACTER(64)  NOT NULL,              \
         amount          BIGINT         NOT NULL,              \
         priceN          INT            NOT NULL,              \
         priceD          INT            NOT NULL,              \
         flags           INT            NOT NULL,              \
         price           BIGINT         NOT NULL,              \
         PRIMARY KEY (accountID, sequence)                     \
         );";

    OfferFrame::OfferFrame()
    {
        mEntry.type(OFFER);
    }
    OfferFrame::OfferFrame(const LedgerEntry& from) : EntryFrame(from)
    {

    }

    void OfferFrame::from(const Transaction& tx) 
    {
        mEntry.type(OFFER);
        mEntry.offer().accountID = tx.account;
        mEntry.offer().amount = tx.body.createOfferTx().amount;
        mEntry.offer().price = tx.body.createOfferTx().price;
        mEntry.offer().sequence = tx.body.createOfferTx().sequence;
        mEntry.offer().takerGets = tx.body.createOfferTx().takerGets;
        mEntry.offer().takerPays = tx.body.createOfferTx().takerPays;
        mEntry.offer().flags = tx.body.createOfferTx().flags;
    }

    void OfferFrame::calculateIndex()
    {
        SHA256 hasher;
        hasher.add(mEntry.offer().accountID);
        // TODO: fix this (endian), or remove index altogether
        hasher.add(ByteSlice(&mEntry.offer().sequence, sizeof(mEntry.offer().sequence)));
        mIndex = hasher.finish();
    }

    Price OfferFrame::getPrice() const
    {
        return mEntry.offer().price;
    }
    
    int64_t OfferFrame::getAmount() const
    {
        return mEntry.offer().amount;
    }

    uint256 const& OfferFrame::getAccountID() const
    {
        return mEntry.offer().accountID;
    }

    Currency& OfferFrame::getTakerPays()
    {
        return mEntry.offer().takerPays;
    }
    Currency& OfferFrame::getTakerGets()
    {
        return mEntry.offer().takerGets;
    }

    uint32 OfferFrame::getSequence()
    {
        return mEntry.offer().sequence;
    }
    

    // TODO: move this and related SQL code to OfferFrame
    static const char *offerColumnSelector =
        "SELECT accountID,sequence,paysIsoCurrency,paysIssuer,"\
        "getsIsoCurrency,getsIssuer,amount,priceN,priceD,flags FROM Offers";

    bool OfferFrame::loadOffer(const uint256& accountID, uint32_t seq,
        OfferFrame& retOffer, Database& db)
    {
        std::string accStr;
        accStr = toBase58Check(VER_ACCOUNT_ID, accountID);

        soci::session &session = db.getSession();

        soci::details::prepare_temp_type sql = (session.prepare <<
            offerColumnSelector << " where accountID=:id and sequence=:seq",
            use(accStr), use(seq));

        bool res = false;

        loadOffers(sql, [&retOffer, &res](OfferFrame const& offer) {
            retOffer = offer;
            res = true;
        });

        return res;
    }


    void OfferFrame::loadOffers(soci::details::prepare_temp_type &prep,
        std::function<void(OfferFrame const&)> offerProcessor)
    {
        string accountID;
        std::string paysIsoCurrency, getsIsoCurrency, paysIssuer, getsIssuer;

        soci::indicator paysIsoIndicator, getsIsoIndicator;

        OfferFrame offerFrame;

        OfferEntry &oe = offerFrame.mEntry.offer();

        statement st = (prep,
            into(accountID), into(oe.sequence),
            into(paysIsoCurrency, paysIsoIndicator), into(paysIssuer),
            into(getsIsoCurrency, getsIsoIndicator), into(getsIssuer),
            into(oe.amount), into(oe.price.n), into(oe.price.d), into(oe.flags)
            );

        st.execute(true);
        while (st.got_data())
        {
            oe.accountID = fromBase58Check256(VER_ACCOUNT_ID, accountID);
            if (paysIsoIndicator == soci::i_ok)
            {
                oe.takerPays.type(ISO4217);
                strToCurrencyCode(oe.takerPays.isoCI().currencyCode, paysIsoCurrency);
                oe.takerPays.isoCI().issuer = fromBase58Check256(VER_ACCOUNT_ID, paysIssuer);
            }
            else
            {
                oe.takerPays.type(NATIVE);
            }
            if (getsIsoIndicator == soci::i_ok)
            {
                oe.takerGets.type(ISO4217);
                strToCurrencyCode(oe.takerGets.isoCI().currencyCode, getsIsoCurrency);
                oe.takerGets.isoCI().issuer = fromBase58Check256(VER_ACCOUNT_ID, getsIssuer);
            }
            else
            {
                oe.takerGets.type(NATIVE);
            }
            offerProcessor(offerFrame);
            st.fetch();
        }
    }

    void OfferFrame::loadBestOffers(size_t numOffers, size_t offset, const Currency & pays,
        const Currency & gets, vector<OfferFrame>& retOffers, Database& db)
    {
        soci::session &session = db.getSession();

        soci::details::prepare_temp_type sql = (session.prepare <<
            offerColumnSelector);

        std::string getCurrencyCode, b58GIssuer;
        std::string payCurrencyCode, b58PIssuer;

        if (pays.type() == NATIVE)
        {
            sql << " WHERE paysIssuer IS NULL";
        }
        else
        {
            currencyCodeToStr(pays.isoCI().currencyCode, payCurrencyCode);
            b58PIssuer = toBase58Check(VER_ACCOUNT_ID, pays.isoCI().issuer);
            sql << " WHERE paysIsoCurrency=:pcur AND paysIssuer = :pi", use(payCurrencyCode), use(b58PIssuer);
        }

        if (gets.type() == NATIVE)
        {
            sql << " AND getsIssuer IS NULL";
        }
        else
        {
            currencyCodeToStr(gets.isoCI().currencyCode, getCurrencyCode);
            b58GIssuer = toBase58Check(VER_ACCOUNT_ID, gets.isoCI().issuer);

            sql << " AND getsIsoCurrency=:gcur AND getsIssuer = :gi", use(getCurrencyCode), use(b58GIssuer);
        }
        sql << " order by price,sequence,accountID limit :o,:n", use(offset), use(numOffers);

        loadOffers(sql, [&retOffers](OfferFrame const &of)
        {
            retOffers.push_back(of);
        });
    }

    void OfferFrame::loadOffers(const uint256& accountID, std::vector<OfferFrame>& retOffers, Database& db)
    {
        soci::session &session = db.getSession();

        std::string accStr;
        accStr = toBase58Check(VER_ACCOUNT_ID, accountID);

        soci::details::prepare_temp_type sql = (session.prepare <<
            offerColumnSelector << " WHERE accountID=:id", use(accStr));

        loadOffers(sql, [&retOffers](OfferFrame const &of)
        {
            retOffers.push_back(of);
        });
    }

    void OfferFrame::storeDelete(LedgerDelta &delta, Database& db)
    {
        std::string b58AccountID = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().accountID);

        db.getSession() <<
            "DELETE FROM Offers WHERE accountID=:id AND sequence=:s",
            use(b58AccountID), use(mEntry.offer().sequence);

        delta.deleteEntry(*this);
    }

    int64_t OfferFrame::computePrice() const
    {
        return bigDivide(mEntry.offer().price.n, OFFER_PRICE_DIVISOR,
            mEntry.offer().price.d);
    }

    void OfferFrame::storeChange(LedgerDelta &delta, Database& db)
    {
        std::string b58AccountID = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().accountID);

        soci::statement st = (db.getSession().prepare <<
            "UPDATE Offers SET amount=:a, priceN=:n, priceD=:D, price=:p WHERE accountID=:id AND sequence=:s",
            use(mEntry.offer().amount),
            use(mEntry.offer().price.n), use(mEntry.offer().price.d),
            use(computePrice()), use(b58AccountID), use(mEntry.offer().sequence));

        st.execute(true);

        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("could not update SQL");
        }

        delta.modEntry(*this);
    }

    void OfferFrame::storeAdd(LedgerDelta &delta, Database& db)
    {
        std::string b58AccountID = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().accountID);

        soci::statement st(db.getSession().prepare << "select 1");

        if(mEntry.offer().takerGets.type()==NATIVE)
        {
            std::string b58issuer = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().takerPays.isoCI().issuer);
            std::string currencyCode;
            currencyCodeToStr(mEntry.offer().takerPays.isoCI().currencyCode, currencyCode);
            st = (db.getSession().prepare <<
                "INSERT into Offers (accountID,sequence,paysIsoCurrency,paysIssuer,"\
                "amount,priceN,priceP,price,flags) values"\
                "(:v1,:v2,:v3,:v4,:v5,:v6,:v7,:v8,:v9)",
                use(b58AccountID), use(mEntry.offer().sequence), 
                use(b58issuer),use(currencyCode),use(mEntry.offer().amount),
                use(mEntry.offer().price.n), use(mEntry.offer().price.d),
                use(computePrice()),use(mEntry.offer().flags));
            st.execute(true);
        }
        else if(mEntry.offer().takerPays.type()==NATIVE)
        {
            std::string b58issuer = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().takerGets.isoCI().issuer);
            std::string currencyCode;
            currencyCodeToStr(mEntry.offer().takerGets.isoCI().currencyCode, currencyCode);
            st = (db.getSession().prepare <<
                "INSERT into Offers (accountID,sequence,getsIsoCurrency,getsIssuer,"\
                "amount,priceN,priceD,price,flags) values"\
                "(:v1,:v2,:v3,:v4,:v5,:v6,:v7,:v8,:v9)",
                use(b58AccountID), use(mEntry.offer().sequence),
                use(b58issuer), use(currencyCode), use(mEntry.offer().amount),
                use(mEntry.offer().price.n), use(mEntry.offer().price.d),
                use(computePrice()), use(mEntry.offer().flags));
            st.execute(true);
        }
        else
        {
            std::string b58PaysIssuer = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().takerPays.isoCI().issuer);
            std::string paysIsoCurrency, getsIsoCurrency;
            currencyCodeToStr(mEntry.offer().takerPays.isoCI().currencyCode, paysIsoCurrency);
            std::string b58GetsIssuer = toBase58Check(VER_ACCOUNT_ID, mEntry.offer().takerGets.isoCI().issuer);
            currencyCodeToStr(mEntry.offer().takerGets.isoCI().currencyCode, getsIsoCurrency);
            st = (db.getSession().prepare <<
                "INSERT into Offers (accountID,sequence,"\
                "paysIsoCurrency,paysIssuer,getsIsoCurrency,getsIssuer,"\
                "amount,priceN,priceD,price,flags) values "\
                "(:v1,:v2,:v3,:v4,:v5,:v6,:v7,:v8,:v9,:v10,:v11)",
                use(b58AccountID), use(mEntry.offer().sequence),
                use(paysIsoCurrency), use(b58PaysIssuer), use(getsIsoCurrency), use(b58GetsIssuer),
                use(mEntry.offer().amount),
                use(mEntry.offer().price.n), use(mEntry.offer().price.d),
                use(computePrice()), use(mEntry.offer().flags));
            st.execute(true);
        }

        if (st.get_affected_rows() != 1)
        {
            throw std::runtime_error("could not update SQL");
        }

        delta.addEntry(*this);
    }

    void OfferFrame::dropAll(Database &db)
    {
        db.getSession() << "DROP TABLE IF EXISTS Offers;";
        db.getSession() << kSQLCreateStatement;
    }
}
