CC = gcc # compilador atual: gcc

# syntax
# regra: dependencias

# nova regra 'all' (compila tudo) que requer a existencia os programas: "controlador", "cliente" e "veiculo". Regra principal que o'make' vai executar.
all: controlador cliente veiculo
	echo "Aplicações compiladas com sucesso!"
	# rm -f *.o # para não ter de estar sempre a apagar os objetos depois

# cria o executável 'controlador' com o comando 'gcc controlador.o -o controlador -pthread'. Juntar o -pthread ao controlador é obrigatório para as threads.
controlador: controlador.o
	$(CC) controlador.o -o controlador -pthread
# cria o objeto 'controlador.o' com o comando 'gcc -c controlador.c' 
controlador.o: controlador.c common.h
	$(CC) -c controlador.c

# cria o executavel 'cliente' com o comando 'gcc cliente.o -o cliente'
cliente: cliente.o
	$(CC) cliente.o -o cliente
# cria o objeto 'cliente.o' com o comando 'gcc -c cliente.c'
cliente.o: cliente.c common.h
	$(CC) -c cliente.c

# cria o executável 'veiculo' com o comando 'gcc veiculo.o -o veiculo'
veiculo: veiculo.o
	$(CC) veiculo.o -o veiculo
# cria o objeto 'veiculo.o' com o comando 'gcc veiculo.c -c'
veiculo.o: veiculo.c common.h
	$(CC) veiculo.c -c

# elimina todos os ficheiros (-f) que acabam em ".o" (objetos) e dos 3 executaveis
clean: 
	rm -f controlador cliente veiculo *.o
