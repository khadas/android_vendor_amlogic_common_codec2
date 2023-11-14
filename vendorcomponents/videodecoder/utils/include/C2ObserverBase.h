#ifndef C2_OBSERVER_BASE_H
#define C2_OBSERVER_BASE_H

#include <iostream>
#include <vector>
#include <algorithm>
#include <memory>

class IC2Observer
{
protected:
    int mComponentState;
    bool mCompHasError;
public:
    virtual ~IC2Observer() {};
    virtual void updateState(const int& state) { mComponentState = state;}
    virtual void updateError(const bool& hasError) { mCompHasError = hasError;}
};


class IC2Subject
{
protected:
    std::vector<std::shared_ptr<IC2Observer>> observers;
public:
    virtual ~IC2Subject() {};
    virtual void addObserver(std::shared_ptr<IC2Observer> observer, const int compState, bool compHasError) {
        observer->updateState(compState);
        observer->updateError(compHasError);
        observers.emplace_back(observer);
    }
    virtual void removeObserver(std::shared_ptr<IC2Observer> observer) {observers.erase(std::remove(observers.begin(), observers.end(), observer), observers.end());}
    virtual void notifyObservers() = 0;
};

#endif