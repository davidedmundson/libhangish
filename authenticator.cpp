/**
 * libhangish
 * Copyright (C) 2015 Tiago Salem Herrmann
 * Copyright (C) 2015 Daniele Rogora
 *
 * This file is part of libhangish.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <QNetworkCookieJar>
#include <QJsonObject>
#include <QJsonDocument>
#include <QFile>
#include <QUrlQuery>
#include <QDomDocument>

#include "authenticator.h"
#include "types.h"


Authenticator::Authenticator(const QString &cookiePath) : mAuthPhase(AUTH_PHASE_INITIAL)
{
    mCookiePath = cookiePath;
    QObject::connect(&mNetworkAccessManager, SIGNAL(finished(QNetworkReply*)), this, SLOT(networkCallback(QNetworkReply *)));
}

void Authenticator::authenticate()
{
    mNetworkAccessManager.setCookieJar(new QNetworkCookieJar(this));
    mSessionCookies.clear();
    QFile cookieFile(mCookiePath);
    if (cookieFile.exists()) {
        cookieFile.open(QIODevice::ReadOnly | QIODevice::Text);
        QString val = cookieFile.readAll();
        cookieFile.close();
        QJsonDocument doc = QJsonDocument::fromJson(val.toUtf8());
        QJsonObject obj = doc.object();
        Q_FOREACH  (QString s, obj.keys()) {
            //TODO: check and delete
            if (s=="ACCOUNT_CHOOSER" || s=="GALX" || s=="GAPS" || s=="LSID" || s=="NID") {
                continue;
            }
            QNetworkCookie tmp;
            tmp.setName(QVariant(s).toByteArray());
            tmp.setValue(obj.value(s).toVariant().toByteArray());
            tmp.setDomain(".google.com");
            mSessionCookies[tmp.name()] = tmp;
        }
        Q_EMIT gotCookies(mSessionCookies);
    } else {
        getGalxToken();
    }
}

void Authenticator::networkCallback(QNetworkReply *reply)
{
    qDebug() << __func__ << mAuthPhase;

    QVariant v = reply->header(QNetworkRequest::SetCookieHeader);
    QList<QNetworkCookie> c = qvariant_cast<QList<QNetworkCookie> >(v);
    Q_FOREACH (QNetworkCookie cookie, c) {
        if (!mSessionCookies.contains(cookie.name())) {
            mSessionCookies[cookie.name()] = cookie;
        }
    }

    if (reply->error() == QNetworkReply::NoError) {
        switch (mAuthPhase) {
        case AUTH_PHASE_GALX_REQUESTED: {
            QVariant v = reply->header(QNetworkRequest::SetCookieHeader);
            QList<QNetworkCookie> cookies = qvariant_cast<QList<QNetworkCookie> >(v);
            Q_FOREACH(QNetworkCookie cookie, cookies) {
                if (cookie.name()=="GALX") {
                    mGALXCookie = cookie;
                    Q_EMIT loginNeeded();
                }
            }
            break;
        }
        case AUTH_PHASE_CREDENTIALS_SENT: {
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()==302) {
                QVariant possibleRedirectUrl =
                    reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
                qDebug() << "redirected" << possibleRedirectUrl.toUrl();
                followRedirection(possibleRedirectUrl.toUrl());
            } else if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()==200) {
                if (amILoggedIn()) {
                    qDebug() << "logged in";
                    saveAuthCookies();
                    Q_EMIT gotCookies(mSessionCookies);
                } else if (reply->url().toString().startsWith(SERVICE_SMS_AUTH_URL)) {
                    mPendingChallenge = SMS;

                    QString ssreply = reply->readAll();

                    int start = ssreply.indexOf("<form id=\"challenge\"");
                    int end = ssreply.indexOf("</form>",start);
                    QString content = ssreply.mid(start, end-start);
                    QDomDocument doc;
                    doc.setContent(content);
                    QDomNode domNode = doc.firstChild();
                    while(!domNode.isNull()) {
                        if(domNode.isElement()) {
                            QDomElement domElement = domNode.toElement();
                            if(!domElement.isNull()) {
                                if (domElement.tagName() == "input") {
                                    QString id = domElement.attribute("id");
                                    if (id == "challengeId") {
                                        mChallengeId = domElement.attribute("value");
                                    } else if (id == "challengeType") {
                                        mChallengeType = domElement.attribute("value");
                                    } else if (id == "gxf") {
                                        mGxf = domElement.attribute("value");
                                    }
                                }
                            }
                        }
                        if (domNode.childNodes().size() > 0) {
                            domNode = domNode.childNodes().at(0);
                        } else {
                            domNode.clear();
                        }
                    }

                    Q_EMIT authFailed(AUTH_NEED_2FACTOR_PIN);
                } else if (reply->url().toString().startsWith(SECONDFACTOR_URL)) {
                    mPendingChallenge = TWO_FACTOR_AUTHENTICATION;
                    qDebug() << "not logged in";
                    //2nd factor auth
                    qDebug() << "Auth failed " << reply->url();
                    QString ssreply = reply->readAll();

                    //find secTok
                    int start = ssreply.indexOf("id=\"secTok\"");
                    start = ssreply.indexOf("'", start)+1;
                    int stop = ssreply.indexOf("'", start);
                    mSecTok = ssreply.mid(start, stop-start);

                    //find timeStmp
                    start = ssreply.indexOf("id=\"timeStmp\"");
                    start = ssreply.indexOf("'", start)+1;
                    stop = ssreply.indexOf("'", start);
                    mTimeStmp = ssreply.mid(start, stop-start);
                    Q_EMIT authFailed(AUTH_NEED_2FACTOR_PIN);
                } else {
                    //Something went wrong
                    qDebug() << "Auth failed " << reply->url();
                    mSessionCookies.clear();
                    Q_EMIT authFailed(AUTH_WRONG_CREDENTIALS);
                }
            } else {
                //Something went wrong
                qDebug() << "Auth failed " << reply->errorString();
                Q_EMIT authFailed(AUTH_UNKNOWN_ERROR, reply->errorString());
            }
            break;
        }
        case AUTH_PHASE_2FACTOR_PIN_SENT: {
            //2nd factor response
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()==200) {
                //TODO: is this really a consistent check for everyone?
                if (amILoggedIn()) {
                    saveAuthCookies();
                    Q_EMIT gotCookies(mSessionCookies);
                } else {
                    Q_EMIT authFailed(AUTH_WRONG_2FACTOR_PIN, reply->url().toString());
                }
            } else if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()==302) {
                QVariant possibleRedirectUrl =
                    reply->attribute(QNetworkRequest::RedirectionTargetAttribute);

                followRedirection(possibleRedirectUrl.toUrl());
            } else {
                qDebug() << "2nd factor error";
                Q_EMIT authFailed(AUTH_UNKNOWN_ERROR);
            }
            break;
        }
        default:
            qDebug() << "Auth phase not recognized";
        }
    } else {
        qDebug() << "ERROR" << mAuthPhase;
        if (mAuthPhase == AUTH_PHASE_GALX_REQUESTED) {
            Q_EMIT authFailed(AUTH_CANT_GET_GALX_TOKEN, reply->errorString());
        }
    }
    reply->deleteLater();
    qDebug() << "end network callback";
}

void Authenticator::followRedirection(QUrl url)
{
    QNetworkRequest req( url );
    req.setRawHeader("User-Agent", USER_AGENT);
    if (!mSessionCookies.isEmpty()) {
        req.setHeader(QNetworkRequest::CookieHeader, QVariant::fromValue(mSessionCookies.values()));
    }
    mNetworkAccessManager.get(req);
}

void Authenticator::getGalxToken()
{
    qDebug() << __func__;

    mSessionCookies.clear();
    mAuthPhase = AUTH_PHASE_GALX_REQUESTED;

    QUrl url(SERVICE_LOGIN_URL);
    QUrlQuery query;

    query.addQueryItem("passive", "true");
    query.addQueryItem("skipvpage", "true");
    query.addQueryItem("continue", "https://talkgadget.google.com/talkgadget/gauth?verify=true");
    query.addQueryItem("authuser", "0");

    url.setQuery(query);

    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", USER_AGENT);
    mNetworkAccessManager.get(req);
}

void Authenticator::saveAuthCookies()
{
    qDebug() << __func__ << mCookiePath;

    QJsonObject obj;
    QMap<QString, QNetworkCookie> liveSessionCookies;
    Q_FOREACH (QNetworkCookie cookie, mSessionCookies) {
        //Let's save the relevant cookies
        if (cookie.name()=="S" || (!cookie.isSessionCookie() && cookie.domain().contains("google.com"))) {
            obj[cookie.name()] = QJsonValue( QString(cookie.value()) );
            liveSessionCookies[cookie.name()] = cookie;
        }
    }
    QJsonDocument doc(obj);
    QFile cookieFile(mCookiePath);
    if ( cookieFile.open(QIODevice::WriteOnly)) {
        cookieFile.write(doc.toJson(), doc.toJson().size());
    }

    cookieFile.close();

    mSessionCookies = liveSessionCookies;
}

void Authenticator::sendCredentials(const QString &uname, const QString &passwd)
{
    qDebug() << __func__;
    mAuthPhase = AUTH_PHASE_CREDENTIALS_SENT;

    QUrlQuery query;
    // TODO: check what fields are actually needed
    query.addQueryItem("GALX", QString(mGALXCookie.value()));
    query.addQueryItem("Email", uname);
    query.addQueryItem("Passwd", passwd);
    query.addQueryItem("bgresponse", "js_disabled");
    query.addQueryItem("dnConn", "0");
    query.addQueryItem("signIn", "Accedi");
    query.addQueryItem("checkedDomains", "youtube");
    query.addQueryItem("PersistentCookie", "yes");
    query.addQueryItem("rmShown", "1");
    query.addQueryItem("pstMsg", "0");
    query.addQueryItem("skipvpage", "true");
    query.addQueryItem("continue", "https://talkgadget.google.com/talkgadget/gauth?verify=true");

    QNetworkRequest req(QUrl ( SERVICE_LOGIN_AUTH_URL ));
    req.setRawHeader("Content-Type", "application/x-www-form-urlencoded");
    mNetworkAccessManager.post(req, query.toString(QUrl::FullyEncoded).toUtf8());
}

void Authenticator::sendChallengePin(const QString &pin)
{
    switch(mPendingChallenge) {
    case SMS:
        sendSMSPin(pin);
        break;
    case TWO_FACTOR_AUTHENTICATION:
        send2ndFactorPin(pin);
        break;
    }
}

void Authenticator::sendSMSPin(const QString &pin)
{
    qDebug() << __func__;

    mAuthPhase = AUTH_PHASE_2FACTOR_PIN_SENT;

    QUrlQuery query;
    // TODO: check what fields are actually needed
    query.addQueryItem("challengeId", mChallengeId);
    query.addQueryItem("challengeType", mChallengeType);
    query.addQueryItem("continue", "https://talkgadget.google.com/talkgadget/gauth?verify=true");
    query.addQueryItem("skipvpage", "true");
    query.addQueryItem("checkedDomains", "youtube");
    query.addQueryItem("pstMsg", "0");
    query.addQueryItem("gxf", mGxf);
    query.addQueryItem("Pin", pin);
    query.addQueryItem("TrustDevice", "true");

    QNetworkRequest req( QUrl( SERVICE_SMS_AUTH_URL ) );
    req.setRawHeader("Content-Type", "application/x-www-form-urlencoded");
    mNetworkAccessManager.post(req, query.toString(QUrl::FullyEncoded).toUtf8());
}

void Authenticator::send2ndFactorPin(const QString &pin)
{
    qDebug() << __func__;

    mAuthPhase = AUTH_PHASE_2FACTOR_PIN_SENT;

    QUrlQuery query;
    // TODO: check what fields are actually needed
    query.addQueryItem("timeStmp", mTimeStmp);
    query.addQueryItem("secTok", mSecTok);
    query.addQueryItem("smsUserPin", pin);
    query.addQueryItem("smsVerifyPin", "Verify");
    query.addQueryItem("smsToken", "");
    query.addQueryItem("checkedConnection", "youtube:73:0");
    query.addQueryItem("checkedDomains", "youtube");
    query.addQueryItem("PersistentCookie", "on");
    query.addQueryItem("PersistentOptionSelection", "1");
    query.addQueryItem("pstMsg", "0");
    query.addQueryItem("skipvpage", "true");

    QNetworkRequest req( QUrl( SECONDFACTOR_URL ) );
    req.setRawHeader("Content-Type", "application/x-www-form-urlencoded");
    mNetworkAccessManager.post(req, query.toString(QUrl::FullyEncoded).toUtf8());
}

bool Authenticator::amILoggedIn() const
{
    int i = 0;
    Q_FOREACH (QNetworkCookie cookie, mSessionCookies) {
        if (cookie.name()=="APISID" ||
            cookie.name()=="HSID" ||
            cookie.name()=="SAPISID" ||
            cookie.name()=="SID" ||
            cookie.name()=="SSID") {
            i++;
        }
    }
    return (i >= 5);
}

void Authenticator::updateCookieFile(const QList<QNetworkCookie> &cookies)
{
    qDebug() << __func__ << mCookiePath;

    QJsonObject obj;
    mSessionCookies.clear();
    QFile cookieFile(mCookiePath);
    if (cookieFile.exists()) {
        cookieFile.open(QIODevice::ReadWrite | QIODevice::Text);
        cookieFile.resize(0);
        Q_FOREACH (QNetworkCookie cookie, cookies) {
            obj[cookie.name()] = QJsonValue( QString(cookie.value()) );
            mSessionCookies[cookie.name()] = cookie;
        }
        QJsonDocument doc(obj);
        QFile cookieFile(mCookiePath);
        if ( cookieFile.open(QIODevice::WriteOnly)) {
            cookieFile.write(doc.toJson(), doc.toJson().size());
        }
        cookieFile.close();
    }
}
