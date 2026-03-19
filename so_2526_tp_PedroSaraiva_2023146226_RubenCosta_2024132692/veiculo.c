#include "common.h"

// Variável para guardar o PID do serviço que o controlador enviou (para debugging)
int service_id_global = -1; // ID do serviço (para logs e mensagens de erro)
char veiculo_fifo_nome[100]; // Nome do pipe DEDICADO deste veículo (./tmp/vec_PID_req)
int fd_veiculo_req = -1; // Descritor do pipe dedicado (onde se lê comandos "entrar/sair")
char fifo_cliente_nome_global[100]; // Nome do pipe privado do cliente (controlador -> cliente)

// acionado quando o Controlador envia SIGUSR1 (Shutdown ou Cancelamento via Admin)
void handler_cancel(int sig) {
    printf("[VEICULO %d] RECEBI CANCELAMENTO (SIGUSR1). A avisar cliente e terminar...\n", getpid());
    fflush(stdout);
    
    // avisa o cliente via Named Pipe privado do cliente
    int fd_cli;
    if ((fd_cli = open(fifo_cliente_nome_global, O_WRONLY)) != -1) {
        MsgControladorCliente msg;
        sprintf(msg.mensagem, "AVISO: O seu servico %d foi CANCELADO pelo Admin!", service_id_global);
        msg.sucesso = 0; 
        write(fd_cli, &msg, sizeof(MsgControladorCliente));
        close(fd_cli);
    }

    // fecha e elimina o pipe privado do veiculo
    if (fd_veiculo_req != -1) close(fd_veiculo_req);
    unlink(veiculo_fifo_nome);
    
    exit(0); // Termina o processo
}

int main (int argc, char *argv[]) {

    int fd_cli_pipe; // descritor para falar com o Cliente
    char cliente_comando[200]; // Buffer para receber o comando 'entrar <destino>'

    MsgControladorCliente msg_cheguei; // Para enviar a mensagem de chegada
    
    // ignora O CTRL+C (SIGINT) que o controlador(admin) pode vir a fazer. Isto impede que o veículo morra instantaneamente quando se faz Ctrl+C no terminal do controladoe. Assim, ele espera que o Controlador lhe envie o SIGUSR1 para morrer com limpeza.
    signal(SIGINT, SIG_IGN);
    
    // Armar o sinal SIGUSR1. Permite que o Admin cancele a viagem a meio ou encerre o sistema de forma limpa
    signal(SIGUSR1, handler_cancel);

    // verificacao dos 6 argumentos extra (para alem do nome)
    if (argc != 7) {
        fprintf(stderr, "[VEICULO] ERRO: Número incorreto de argumentos (%d). Esperado: 6.\n", argc - 1);
        return 1;
    }

    // extracao dos argumentos
    service_id_global = atoi(argv[1]); // Arg 1: ID do Serviço
    int distancia_km = atoi(argv[2]); // Arg 2: Distância a percorrer
    char *local_partida = argv[3];    // Arg 3: Local
    char *username_cliente = argv[4]; // Arg 4: Cliente
    int pid_cliente = atoi(argv[5]);  // Arg 5: PID Cliente
    
    // guarda o nome do pipe do cliente na var global
    strcpy(fifo_cliente_nome_global, argv[6]);

    // Inicio da telemetria (STDOUT -> Pipe Anonimo). Tudo que se escreve aqui (printf) o controlador lê na thread da telemetria via pipe anonimo
    printf("[VEICULO %d] START | Serviço: %d | Cliente: %s | Dist: %dkm | FIFO: %s\n", getpid(), service_id_global, username_cliente, distancia_km, fifo_cliente_nome_global);
    fflush(stdout);
    
    // criacao do pipe privado do veiculo
    sprintf(veiculo_fifo_nome, "./tmp/vec_%d_req", getpid());
    if (mkfifo(veiculo_fifo_nome, 0660) == -1) { // 0660 = Permissões (Dono=RW, Grupo=RW, Outros=---)
        perror("[VEICULO] ERRO ao criar pipe de resposta");
        return 1;
    }

    // abre-se o pipe do Cliente (para escrita) e diz-se que o veiculo chegou ao local
    if ((fd_cli_pipe = open(fifo_cliente_nome_global, O_WRONLY)) == -1) { // fd_cli_pipe é o descritor do pipe privado do cliente
        fprintf(stderr, "[VEICULO %d] ERRO: Nao consegui contactar o cliente %s. A terminar.\n", getpid(), username_cliente);
        unlink(veiculo_fifo_nome);
        return 1;
    }

    // constrói-se a mensagem de chegada, embutindo o nome do pipe privado do veiculo de forma a o cliente conseguir ler e voltar a contactar o veiculo dessa forma
    sprintf(msg_cheguei.mensagem, "Veiculo chegou ao local. Responda com 'entrar <destino>' para o pipe %s", veiculo_fifo_nome);
    msg_cheguei.sucesso = 1;
    write(fd_cli_pipe, &msg_cheguei, sizeof(MsgControladorCliente)); 
    close(fd_cli_pipe); 

    // abri-se o pipe do veículo para leitura
    if ((fd_veiculo_req = open(veiculo_fifo_nome, O_RDONLY)) == -1) {
        perror("[VEICULO] ERRO ao abrir pipe de resposta para leitura");
        unlink(veiculo_fifo_nome);
        return 1;
    }

    // bloqueia ate ler o comando: "entrar <destino>"
    int n = read(fd_veiculo_req, cliente_comando, sizeof(cliente_comando) - 1);
    char destino[50];

    if (n > 0) { // se o comando do cliente tiver +0 bytes
        cliente_comando[n] = '\0';
        
        // extrai o destino do comando
        if (sscanf(cliente_comando, "entrar %s", destino) == 1) {
            // atualiza-se o controlador
            printf("[VEICULO %d] EMBARQUE | Cliente %s embarcou. Destino: %s.\n", getpid(), username_cliente, destino);
            fflush(stdout);
            
            // Fecha-se e reabre-se como O_NONBLOCK. Isto permite ler o pipe sem ficar preso, para detetar o comando "sair"
            close(fd_veiculo_req);
            fd_veiculo_req = open(veiculo_fifo_nome, O_RDONLY | O_NONBLOCK);
            
            float progresso_percentagem = 0.0;
            int viagem_interrompida = 0;
            char cmd_meio[50];
            
            for (int km = 1; km <= distancia_km; km++) {
            
                // A. Simula o tempo (1km = 1s)
                sleep(1); // Simula o tempo (1 km por 1 segundo real)
                
                // B. Calcula e reporta progresso
                float percent = (float)km / distancia_km * 100.0;
                // Reporta apenas quando atinge o próximo múltiplo de 10%
                if (percent >= progresso_percentagem + 10.0 || km == distancia_km) {
                    progresso_percentagem = (int)(percent / 10.0) * 10.0; 
                    if (km == distancia_km) progresso_percentagem = 100.0;
                    // reporta o progresso para o controlador
                    printf("[VEICULO %d] PROGRESSO | %d%% | KM: %d\n", getpid(), (int)progresso_percentagem, km);
                    fflush(stdout);
                }
                
                // C. Tenta ler do pipe (comando "sair"?). Como está NONBLOCK, se não houver nada, retorna -1 e continua.
                int n_meio = read(fd_veiculo_req, cmd_meio, sizeof(cmd_meio)-1);
                
                if (n_meio > 0) {
                    cmd_meio[n_meio] = '\0';
                    if (strncmp(cmd_meio, "sair", 4) == 0) {
                        printf("[VEICULO %d] CLIENTE_SAIU | O cliente saiu no KM: %d.\n", getpid(), km); // controlador percebe por aqui
                        fflush(stdout);
                        viagem_interrompida = 1;
                        break;
                    }
                }
            }
            
            // ultima atualizacao da viagem ao controlador
            if (!viagem_interrompida) {
                printf("[VEICULO %d] FIM_VIAGEM | Cliente %s chegou ao destino.\n", getpid(), username_cliente);
                fflush(stdout);
            }
            
        } else if (strncmp(cliente_comando, "sair", 4) == 0) { // caso o cliente escreva o comando: "sair" na altura do embarque, conta como viagem concluida e 0 km percorridos nessa viagem (tal como funciona quando ele sai a meio da viagem). De outra forma, dava bug
            printf("[VEICULO %d] CLIENTE_SAIU | Cliente cancelou no embarque (sem viajar).\n", getpid());
            fflush(stdout);
        } else {
            printf("[VEICULO %d] Cliente enviou comando invalido: %s. A terminar.\n", getpid(), cliente_comando);
            fflush(stdout);
        }
    } else {
        printf("[VEICULO %d] Cliente nao respondeu ou desconectou-se. A terminar.\n", getpid());
        fflush(stdout);
    }
    
    close(fd_veiculo_req);
    unlink(veiculo_fifo_nome);
    return 0;
}
