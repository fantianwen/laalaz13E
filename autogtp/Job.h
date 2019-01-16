/*
    This file is part of Leela Zero.
    Copyright (C) 2017-2018 Marco Calignano

    Leela Zero is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Leela Zero is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Leela Zero.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef JOB_H
#define JOB_H

#include "Result.h"
#include "Order.h"
#include <QObject>
#include <QAtomicInt>
#include <QTextStream>
class Management;
using VersionTuple = std::tuple<int, int, int>;

class Job : public QObject {
    Q_OBJECT
public:
    enum {
        RUNNING = 0,
        FINISHING,
        STORING
    };
    enum {
        Production = 0,
        Validation
    };
    Job(QString gpu, Management *parent);
    ~Job() = default;
    virtual Result execute() = 0;
    virtual void init(const Order &o);
    void finish() { m_state.store(FINISHING); }
    void store() {
        m_state.store(STORING);
    }

protected:
    QAtomicInt m_state;
    QString m_option;
    QString m_gpu;
    int m_moves;
    VersionTuple m_leelazMinVersion;
    Management *m_boss;
};


class ProductionJob : public Job {
    Q_OBJECT
public:
    ProductionJob(QString gpu, Management *parent);
    ~ProductionJob() = default;
    void init(const Order &o);
    Result execute();
private:
    QString m_network;
    QString m_sgf;
    bool m_debug;
};

class ValidationJob : public Job {
    Q_OBJECT
public:
    ValidationJob(QString gpu, Management *parent);
    ~ValidationJob() = default;
    void init(const Order &o);
    Result execute();
private:
    QString m_firstNet;
    QString m_secondNet;
    QString m_sgfFirst;
    QString m_sgfSecond;
};

class WaitJob : public Job {
    Q_OBJECT
public:
    WaitJob(QString gpu, Management *parent);
    ~WaitJob() = default;
    void init(const Order &o);
    Result execute();
private:
    int m_minutes;
};

#endif // JOB_H
