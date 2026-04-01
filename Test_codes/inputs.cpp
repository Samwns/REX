//tambem e possivel fazer entrada  e saida de dados em C++

//cabecario

//bibliotecas
#include <iostream>
#include <string>
#include <stdio.h>
#include <windows.h>

//função principal
int main(){
    //definimos algumas variaveis
    std::string nome = "";
    int idade = 0;
    float peso = 0;
    float altura = 0;

    // agora vamos incia  um imput para cada dado  que  foi armazenado ali

    std::cout << "Ola, Por Favor Digite Seu Nome: ";

    //agora  vamo chama um input  com "std::cin" ele joga o dado fornecido pelo usuario para  uma variavel
    // atualizando o valor dela
    
    std::getline(std::cin, nome); // Lê o nome inteiro

    //agora vamos fazer  isso para as  outras variaveis

    std::cout << "Digite Sua  Idade: ";

    std::cin >> idade;

    std::cout << "Digite Seu Peso: ";

    std::cin >> peso;

    std::cout << "Digite Sua Altura: ";

    std::cin >> altura;

    //agora vamos exibir os dados que foram coletados

    std::cout << "Você se chama: " << nome << "\n";
    std::cout << "Sua idade é: " << idade << "\n";
    std::cout << "Você Pesa: " << peso << "\n";
    std::cout << "Você Mede: " << altura << "\n";


     /*
        == → igual

        = → diferente

        > → maior

        < → menor

        >= → maior ou igual

        <= → menor ou igual

     */




    if (peso >= 80) {
        std::cout << "Voce tem:\n" << peso << "Voce é Imenso\n";
    } else {
         std::cout << "Voce não e obeso\n"; //ae não é obeso
    } // kkkkkkkkkkkkkkkkkkkkkk   
    
    
    
    system("pause");
    return 0;

}