#!/usr/bin/env python3
"""Export a simple LSTM model as TorchScript for torch9 testing.

Usage:
    python export_lstm.py [input_size] [hidden_size] [num_layers]

    Defaults: input_size=10, hidden_size=20, num_layers=1
    Output: lstm_{input_size}_{hidden_size}_{num_layers}.pt

Example:
    python export_lstm.py           # -> lstm_10_20_1.pt
    python export_lstm.py 32 64 2   # -> lstm_32_64_2.pt
"""

import sys
import torch
import torch.nn as nn


class LSTMWrapper(nn.Module):
    def __init__(self, input_size: int, hidden_size: int, num_layers: int = 1):
        super().__init__()
        self.lstm = nn.LSTM(input_size, hidden_size, num_layers, batch_first=True)

    def forward(self, x: torch.Tensor):
        output, (h_n, c_n) = self.lstm(x)
        return output, h_n, c_n


def main():
    input_size = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    hidden_size = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    num_layers = int(sys.argv[3]) if len(sys.argv) > 3 else 1

    model = LSTMWrapper(input_size, hidden_size, num_layers)
    model.eval()

    scripted = torch.jit.script(model)
    filename = f"lstm_{input_size}_{hidden_size}_{num_layers}.pt"
    scripted.save(filename)
    print(f"Saved TorchScript model to {filename}")
    print(f"  input_size={input_size}, hidden_size={hidden_size}, num_layers={num_layers}")


if __name__ == "__main__":
    main()
