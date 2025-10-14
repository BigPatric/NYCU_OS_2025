import os

def read_and_sort(filename):
    with open(filename, 'r') as f:
        lines = f.readlines()
    if len(lines) < 2:
        raise ValueError("Input file must have at least two lines")
    n = int(lines[0].strip())
    # 讀取第二行，分割成 n 個數字
    numbers = list(map(int, lines[1].strip().split()))
    if len(numbers) != n:
        raise ValueError(f"Expected {n} numbers, but got {len(numbers)}")
    return sorted(numbers)  # 按數字大小排序

def write_output_std(sorted_list, filename="output_std.txt"):
    # 輸出在同一行，用空格分隔
    with open(filename, 'w') as f:
        f.write(' '.join(map(str, sorted_list)) + '\n')

def compare_files(std_file, output_file):
    with open(std_file, 'r') as f:
        std = f.read().strip().split()
    with open(output_file, 'r') as f:
        output = f.read().strip().split()

    if std == output:
        print(f"{output_file}: 完全一致！")
    else:
        print(f"{output_file}: 不一致。")
        # 顯示前 5 筆不同的地方
        for i, (a, b) in enumerate(zip(std, output)):
            if a != b:
                print(f"  第 {i+1} 個數字不同: std: {a} | output: {b}")
            if i >= 4:
                break
        if len(std) > len(output):
            print(f"  std 多了 {len(std) - len(output)} 個數字")
        elif len(output) > len(std):
            print(f"  output 多了 {len(output) - len(std)} 個數字")

if __name__ == "__main__":
    # 產生 output_std.txt
    sorted_input = read_and_sort("input.txt")
    write_output_std(sorted_input, "output_std.txt")
    print("已產生 output_std.txt")

    # 逐一比對 output_1.txt ~ output_8.txt
    for i in range(1, 9):
        output_file = f"output_{i}.txt"
        if os.path.exists(output_file):
            compare_files("output_std.txt", output_file)
        else:
            print(f"{output_file} 不存在，略過。")
