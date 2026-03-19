#include "common.h"

// variaveis globais apenas para pode-las chamar na funcao clean
int fd_cli; // Descritor do nosso pipe privado (leitura)
char fifo_privado_nome[100]; // Nome do ficheiro do pipe privado ("./tmp/cli_PID")
char my_username[MAX_USERNAME]; // Nome do utilizador (para o aviso de saída)
int fd_sv; // Descritor do pipe público do servidor (escrita)

// variaveis para comunicacao: cliente->veiculo
char veiculo_pipe_name[100]; // nome do pipe de resposta do veiculo
int fd_veiculo = -1;         // descriptor do pipe do veiculo

// Função de clean para apagar o pipe público quando o cliente morrer pelo comando "terminar" e para sair (com Ctrl+C)
void clean(int sig) {
    if (sig != 0) { 
        // Se foi um sinal (Ctrl+C), avisamos o servidor antes de morrer
        MsgClienteControlador msg;
        msg.tipo = TIPO_CMD;
        msg.pid = getpid();
        strcpy(msg.username, my_username);
        strcpy(msg.comando, "terminar"); // Envia comando 'terminar' para o servidor nos remover da lista

        // Envia o aviso silencioso
        write(fd_sv, &msg, sizeof(MsgClienteControlador));
        
        // Pequena pausa para garantir que o servidor processa antes de fecharmos os canais
        usleep(150000);
    }

    printf("\n   [CLIENTE %d] A terminar...\n", getpid());
    // Fecha os descritores abertos
    close(fd_cli);
    close(fd_sv);
    unlink(fifo_privado_nome); // Apaga o ficheiro do pipe
    exit(0);
}

int main(int argc, char *argv[]) {
    MsgClienteControlador msg_enviar;
    MsgControladorCliente msg_resposta;
    char buffer_comandos[200];

    // validacao de argumentos de logins
    if (argc != 2) {
        printf("ERRO - Utilizacao: ./cliente <username>\n");
        exit(1);
    }
    
    // guarda o username para uso global
    strcpy(my_username, argv[1]);
    
    // ativar o sinal SIGINT (Ctrl+C) para chamar a função clean
    signal(SIGINT, clean);
    
    // prepara dados do cliente para enviar ao controlador
    msg_enviar.pid = getpid();
    strcpy(msg_enviar.username, argv[1]);
    
    printf("   [CLIENTE %d] A arrancar como '%s'...\n", msg_enviar.pid, msg_enviar.username);

    // cria o pipe privado de resposta
    sprintf(fifo_privado_nome, FIFO_CLIENTE, msg_enviar.pid); // guarda em 'fifo_privado_nome', FIFO_CLIENTE+PID (Ex: cli_1234)
    if (mkfifo(fifo_privado_nome, 0660) == -1) { // fifo privado criado aqui. 0660 = Permissões (Dono=RW, Grupo=RW, Outros=---)
        perror("   [CLIENTE] Aviso: mkfifo privado");
    }

    // abre pipe publico (para enviar tudo)
    if ((fd_sv = open(FIFO_CONTROLADOR, O_WRONLY)) == -1) {
        perror("   [CLIENTE] Erro: O controlador nao esta a correr!");
        unlink(fifo_privado_nome);
        exit(2);
    }
    
    // envia pedido de Login (TIPO_LOGIN)
    printf("   [CLIENTE %d] A enviar pedido de LOGIN...\n", msg_enviar.pid);
    msg_enviar.tipo = TIPO_LOGIN;
    write(fd_sv, &msg_enviar, sizeof(MsgClienteControlador));
    
    // abre pipe privado (para ler tudo) abre-se para leitura. Esta chamada vai bloquear até o controlador envie a resposta ao login
    if ((fd_cli = open(fifo_privado_nome, O_RDWR)) == -1) {
        perror("   [CLIENTE] Erro: open privado");
        // O controlador pode já ter fechado, por isso limpamos e saímos
        close(fd_sv);
        unlink(fifo_privado_nome);
        exit(3);
    }

    // le resposta de Login
    if (read(fd_cli, &msg_resposta, sizeof(MsgControladorCliente)) > 0) {
        printf("   [CONTROLADOR-SV] %s\n", msg_resposta.mensagem);

        // Se o login falhou (sucesso == 0), o cliente termina.
        if (msg_resposta.sucesso == 0) {
            printf("   [CLIENTE] Falha no login. A terminar.\n");
            close(fd_sv);
            clean(0); // Chama a limpeza (que faz exit e elimina o fifo privado do cliente)
        }
    } else {
        printf("   [CLIENTE] Erro a ler resposta de login. Controlador morreu?\n");
        exit(5);
    }
    
    printf("\nLogin OK. Escreva comandos ('terminar' para sair):\n");
    
    // 1. Imprime o prompt inicial UMA VEZ antes do loop
    printf("\n >>> ");
    fflush(stdout);

    // loop principal com SELECT
    // O select vai vigiar duas fontes de dados:
    // 1. O Teclado (stdin) - O utilizador pode escrever a qualquer momento.
    // 2. O Pipe Privado (fd_cli) - O servidor/veículo podem enviar mensagens a qualquer momento.
    
    fd_set fds_para_ler; // O conjunto de FDs que o select() vai vigiar
    int max_fd;          // O valor do FD mais alto

    while (1) {
        // preparar o conjunto de FDs
        FD_ZERO(&fds_para_ler);          // Limpa o conjunto
        FD_SET(0, &fds_para_ler);        // Adiciona o STDIN (teclado, FD=0)
        FD_SET(fd_cli, &fds_para_ler);   // Adiciona o PIPE PRIVADO (respostas)
        
        // determinar o FD máximo (FDs vão de 0 a max_fd) para o select saber até onde iterar
        max_fd = (fd_cli > 0) ? fd_cli : 0; 
        
        // chamar bloqueante SELECT
        int res = select(max_fd + 1, &fds_para_ler, NULL, NULL, NULL); // O programa dorme aqui até que ALGUÉM tenha dados (Teclado OU Pipe)

        if (res == -1) {
            if (errno == EINTR) { continue; } // Ignora interrupções de ^C (o clean trata disso)
            perror("   [CLIENTE] Erro no select()");
            break; 
        }

        // le do PIPE PRIVADO (fd_cli)
        if (FD_ISSET(fd_cli, &fds_para_ler)) {
            if (read(fd_cli, &msg_resposta, sizeof(MsgControladorCliente)) > 0) { // le a resposta do controlador/veículo e guarda em 'msg_resposta'
                
                printf("   [CONTROLADOR-SV] %s\n", msg_resposta.mensagem); // Imprime a resposta
                
                // apenas sai do while (sem ser pelo controlador ter-se desconectado) caso o cliente faça logout 
                if (strcmp(msg_resposta.mensagem, "Sessao terminada com sucesso. Ate logo!") == 0) {
                     break; // Sai do loop para a limpeza
                }
                
                if (strstr(msg_resposta.mensagem, "Veiculo chegou ao local") != NULL) { // caso a mensagem de vinda contenha: 'Veiculo chegou ao local'   
                    //Aqui cria-se um ponteiro que vai apontar para o inicio da palavra: "pipe " na mensagem vindo do controlador/veiculo
                    char *pipe_start = strstr(msg_resposta.mensagem, "pipe "); // ponteiro que aponta para o inicio do "pipe " na mensagem
                    
                    if (pipe_start != NULL) { // se nao for null
                        pipe_start += 5; // Avança o ponteiro para o início do nome do pipe. Salta "pipe " -> 5 casas para a frente desde o inicio
                        
                        strcpy(veiculo_pipe_name, pipe_start); // Copia o nome do pipe para a variável global
                        
                        printf("   [CLIENTE] PRONTO PARA EMBARQUE. Digite 'entrar <destino>'.\n");
                    }
                } else if (strstr(msg_resposta.mensagem, "CHEGOU AO DESTINO") != NULL || strstr(msg_resposta.mensagem, "Viagem terminada") != NULL) {
                    // Limpa o nome do pipe do veículo, indicando que já não há carro
                    veiculo_pipe_name[0] = '\0';
                } else if (strstr(msg_resposta.mensagem, "Servidor a desligar") != NULL) {
                    break; // Sai do loop para a limpeza caso o servidor se tenha desconectado
                }
                
                printf("\n >>> ");
                fflush(stdout);
                
            } else {
                // O read() deu 0 ou -1 (Controlador morreu inesperadamente)
                printf("\n   [CLIENTE] O controlador desconectou-se!\n");
                break; 
            }
        }
        
        // le do STDIN (teclado)
        if (FD_ISSET(0, &fds_para_ler)) {
            fgets(buffer_comandos, sizeof(buffer_comandos), stdin); // guarda em 'buffer_comandos' o que o cliente escreve
            buffer_comandos[strcspn(buffer_comandos, "\n")] = 0; // remove o '\n' (enter que o cliente fez) no final do buffer. O fgets lê o Enter como '\n' daí ser necessário remover.

            if (strlen(buffer_comandos) == 0) { // caso seja um enter vaziu entra no if
                printf("\n >>> ");
                fflush(stdout);
                continue; // Ignora ENTER
            }
            
            if (strcmp(buffer_comandos, "clear") == 0) { // comando opcional: "clear" para apagar a consola
                system("clear"); // comando do sistema operativo para apagar a consola
                printf("Login OK. Escreva comandos ('terminar' para sair):\n");
                printf("\n >>> ");
                fflush(stdout);
                continue; // nao faz mais nada, volta ao inicio do loop
            }
            
            // saca apenas a primeira palavra individual e interpreta-o como o comando principal
            char cmd_primario[20];
            sscanf(buffer_comandos, "%s", cmd_primario);
            
            // comandos direcionados ao veiculo APENAS: "entrar" e "sair"
            if (strcmp(cmd_primario, "entrar") == 0 || strcmp(cmd_primario, "sair") == 0) {
            
                // caso seja "entrar"
                if (strcmp(cmd_primario, "entrar") == 0) {
                    char temp_dest[100];
                    // Tenta ler a segunda palavra do buffer (destino da viagem)
                    if (sscanf(buffer_comandos, "%*s %s", temp_dest) != 1) { // %*s ignora a primeira palavra ("entrar"), %s lê a segunda
                        printf("   [CLIENTE] ERRO: Falta o destino. Use: entrar <destino>\n");
                        printf("\n >>> ");
                        fflush(stdout);
                        continue; // Volta ao início do loop, não envia nada
                    }
                }
                
                // verifica se o cliente tem um veículo para contactar
                if (veiculo_pipe_name[0] == '\0') {
                     printf("   [CLIENTE] ERRO: Nao ha veiculo para contactar. Aguarde a chegada.\n");
                     printf("\n >>> ");
                     fflush(stdout);
                     continue;
                }
                
                // abre o pipe temporario do veiculo (para escrever o comando)
                if ((fd_veiculo = open(veiculo_pipe_name, O_WRONLY)) == -1) { // abre o pipe único do seu veiculo
                    perror("   [CLIENTE] ERRO ao abrir pipe do veiculo. Veiculo morreu?");
                    veiculo_pipe_name[0] = '\0'; // Limpa o nome do pipe para resetar o estado de embarque 
                    printf("\n >>> ");
                    fflush(stdout);
                    continue;
                }
                
                // envia o comando (ex: "entrar Coimbra") DIRETAMENTE para o veiculo
                write(fd_veiculo, buffer_comandos, strlen(buffer_comandos) + 1); // +1 para o '\0'
                close(fd_veiculo);
                
                printf("\n >>> ");
                fflush(stdout);
                
                // O cliente regressa ao select para esperar a telemetria/resposta
            }
            // comandos direncionados ao controlador: agendar, terminar, etc.
            else {
                // Prepara a mensagem do cliente para o controlador
                msg_enviar.tipo = TIPO_CMD;
                strcpy(msg_enviar.comando, buffer_comandos);
                strcpy(msg_enviar.username, argv[1]);

                write(fd_sv, &msg_enviar, sizeof(MsgClienteControlador)); // Envia o comando para o pipe público
            }
        }
    }

    // sai do loop apenas quando ha um break (1-erro nos selects, 2-admin escreveu 'terminar' ou faz CTRL+C, 3-cliente escreveu 'terminar', 4-sv desligou-se de repente)
    clean(0);
    return 0;
}
