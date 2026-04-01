#include <iostream>
#include <string>
#include <vector>

class Animal {
public:
    std::string name;
    int age;
    
    Animal(const std::string& n, int a) : name(n), age(a) {}
    
    virtual std::string speak() const {
        return "...";
    }
    
    void print() const {
        std::cout << name << " (age " << age << "): " << speak() << "\n";
    }
};

class Dog : public Animal {
public:
    Dog(const std::string& n, int a) : Animal(n, a) {}
    
    std::string speak() const override {
        return "Woof!";
    }
};

int main() {
    std::vector<Animal*> animals;
    animals.push_back(new Dog("Rex", 3));
    animals.push_back(new Dog("Buddy", 5));
    
    for (auto* a : animals) {
        a->print();
    }
    
    std::cout << "Done!\n";
    return 0;
}
