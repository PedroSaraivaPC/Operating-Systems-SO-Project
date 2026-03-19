#ifndef COMMON_H
#define COMMON_H

// --- Bibliotecas para os 3 ficheiros principais ---
#include <stdio.h> 
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/select.h>
#include <pthread.h>
#include <errno.h>

// --- Constantes e limites ---
#define MAX_USERNAME 50 // Tamanho máximo do nome do utilizador (segurança no buffer)
#define MAX_UTILIZADORES 30 // Limite max de utilizadores ao mesmo tempo (requerido no enunciado)
#define MAX_SERVICOS 100 // Limite max de serviços na lista de servicos do controlador

// --- Named pipes ---

// 1 - Pipe PÚBLICO: Cliente -> Controlador | Todos os cliente escrevem neste pipe enquanto o controlador lê sequencialmente 1 por 1
#define FIFO_CONTROLADOR "./tmp/fifo_controlador"

// 2. Pipe PRIVADO: Controlador -> Cliente | Um pipe por cliente, de forma a cada um receber a sua resposta privada vinda do controlador. o '%d' é para guardar o PID individual do cliente, assim dá para indetificar o pipe de cada cliente em específico.
#define FIFO_CLIENTE "./tmp/cli_%d"


// --- structs comunicacao controlador <-> cliente ---

// Tipos de mensagem que o cliente pode enviar
typedef enum {
    TIPO_LOGIN, // Pedido inicial de entrada no sistema
    TIPO_CMD // Qualquer outro comando (menos entrar e sair, estes 2 comando são os únicos que se destinam ao veículo)
} MsgTipo;

// Estrutura da mensagem enviada pelo CLIENTE (escrita no pipe público)
typedef struct {
    MsgTipo tipo;                 // Para o controlador saber o que fazer (login ou outro comando)
    int pid;                      // PID do cliente (para saber o nome do pipe privado)
    char username[MAX_USERNAME];  // Usado apenas se tipo == TIPO_LOGIN
    char comando[200];            // Usado apenas se tipo == TIPO_CMD
} MsgClienteControlador;

// Estrutura da mensagem enviada pelo CONTROLADOR (scrita no pipe privado)
typedef struct {
    int sucesso; // 1 para OK, 0 para ERRO (criado para o controlador poder informar o cliente se o login falhou
    char mensagem[2048]; // mensagem de resposta privada do controlador ao cliente 
} MsgControladorCliente;


// structs CONTROLADOR ---

// estado do serviço
typedef enum {
    AGENDADO, // Serviço criado, aguarda que a hora simulada chegue
    EM_VIAGEM, // Veículo atribuído, processo filho criado e em movimento
    CANCELADO, // Quando é cancelado pelo utilizador (antes do inicio da viagem) ou pelo admin (a qualquer altura)
    CONCLUIDO // Terminou com sucesso (chegou ao destino ou utilizador saiu mais cedo)
} EstadoServico;

// representa um servico na lista de servicos do controlador
typedef struct {
    int id;                     // ID único do serviço (1, 2, 3...)
    char username_cliente[MAX_USERNAME]; // Cliente que agendou
    int pid_cliente;            // PID do Cliente (para contacto)
    int hora_pedido;            // Tempo simulado quando o pedido foi feito
    int hora_inicio;            // Hora simulada a que o veículo deve arrancar (do cliente)
    char local_partida[50];     // Local de partida (apenas informativo)
    int distancia_km;           // Distância total da viagem
    EstadoServico estado;       // Estado atual do serviço
    int pid_veiculo;            // PID do processo 'veiculo' (após lançamento)
    int distancia_percorrida;   // Telemetria: Atualizado em tempo real pela thread de telemetria
} Servico;

// tabela da frota ativa (Thread telemetria <-> Processo Veículo)
typedef struct {
    int pid_veiculo; // O processo veiculo: Quem escreve a telemetria (stdout)
    int fd_leitura_anony_pipe; // O descritor de leitura do pipe anónimo
    pthread_t tid; // A thread telemetria para cada veiculo: Quem lê os dados. Guardado para controlo (detach)
    int servico_id; // Link para a tabela de Serviços (para saber que serviço atualizar)
} FrotaAtiva;

#endif
